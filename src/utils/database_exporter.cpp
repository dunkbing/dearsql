#include "utils/database_exporter.hpp"

#include "database/mysql/mysql_internal.hpp"

#include <cstddef>
#include <fstream>
#include <nfd.h>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <vector>

namespace {

    using mysql_internal::MysqlResPtr;

    // Rows are accumulated into extended INSERTs up to this size. Same reasoning
    // as the importer's batch cap: comfortably under the usual max_allowed_packet
    // so a dump this writes can always be read back.
    constexpr std::size_t kMaxInsertBytes = 1024 * 1024;

    // What mysqldump emits. FOREIGN_KEY_CHECKS=0 is the load-bearing one: without
    // it the tables restore in whatever order they were written and the
    // constraints reject them.
    constexpr const char* kPreamble =
        "/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;\n"
        "/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;\n"
        "/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;\n"
        "/*!40101 SET NAMES utf8mb4 */;\n"
        "/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;\n"
        "/*!40103 SET TIME_ZONE='+00:00' */;\n"
        "/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;\n"
        "/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;\n"
        "/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;\n"
        "\n";

    constexpr const char* kEpilogue =
        "\n"
        "/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;\n"
        "/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;\n"
        "/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;\n"
        "/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;\n"
        "/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;\n"
        "/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;\n"
        "/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;\n";

    std::string quoteIdentifier(const std::string& name) {
        std::string quoted = "`";
        for (const char c : name) {
            if (c == '`') {
                quoted += '`'; // a backtick in an identifier is doubled
            }
            quoted += c;
        }
        quoted += '`';
        return quoted;
    }

    // Escaping goes through the server connection so it honours the connection
    // charset and is binary safe. The string-based row representation used
    // elsewhere cannot do this: it renders NULL as a sentinel value that a real
    // column could legitimately contain.
    std::string quoteValue(MYSQL* conn, const char* data, const unsigned long length) {
        if (!data) {
            return "NULL";
        }
        std::string escaped(length * 2 + 1, '\0');
        const unsigned long written =
            mysql_real_escape_string(conn, escaped.data(), data, length);
        escaped.resize(written);
        return "'" + escaped + "'";
    }

    void execute(MYSQL* conn, const std::string& sql) {
        if (mysql_real_query(conn, sql.c_str(), sql.size()) != 0) {
            throw std::runtime_error(mysql_error(conn));
        }
    }

    // SHOW CREATE TABLE and SHOW CREATE VIEW both return the DDL in column 1.
    std::string showCreate(MYSQL* conn, const std::string& sql) {
        execute(conn, sql);
        const MysqlResPtr res(mysql_store_result(conn));
        if (!res) {
            throw std::runtime_error(mysql_error(conn));
        }
        MYSQL_ROW row = mysql_fetch_row(res.get());
        if (!row || mysql_num_fields(res.get()) < 2 || !row[1]) {
            throw std::runtime_error("Unexpected result from: " + sql);
        }
        return row[1];
    }

    // Ask the server what the database contains rather than reading the sidebar's
    // model. That model is populated lazily when a node is expanded, so exporting
    // a database the user never opened would otherwise produce a dump with a
    // preamble, an epilogue and nothing in between.
    void listObjects(MYSQL* conn, std::vector<std::string>& tables,
                     std::vector<std::string>& views) {
        execute(conn, "SHOW FULL TABLES");
        const MysqlResPtr res(mysql_store_result(conn));
        if (!res) {
            throw std::runtime_error(mysql_error(conn));
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get())) != nullptr) {
            if (!row[0]) {
                continue;
            }
            // Column 1 is Table_type: "BASE TABLE", "VIEW", or "SYSTEM VIEW".
            const std::string type = row[1] ? row[1] : "";
            if (type == "VIEW") {
                views.emplace_back(row[0]);
            } else if (type == "BASE TABLE") {
                tables.emplace_back(row[0]);
            }
        }
    }

    bool cancelled(const DatabaseExporter::Progress& progress, const std::stop_token& stopToken) {
        return stopToken.stop_requested() ||
               progress.cancelRequested.load(std::memory_order_relaxed);
    }

    // Streams a table's rows into extended INSERT statements.
    //
    // Uses mysql_use_result rather than mysql_store_result: the latter buffers the
    // entire table in client memory, which a multi-gigabyte table will not
    // survive. The trade-off is that the connection cannot issue another query
    // until the result is fully consumed, which is fine here because the export
    // owns the session for its whole run.
    std::uint64_t writeTableData(MYSQL* conn, std::ofstream& out, const std::string& tableName,
                                 DatabaseExporter::Progress& progress,
                                 const std::stop_token& stopToken, bool& stopped) {
        const std::string quotedName = quoteIdentifier(tableName);
        execute(conn, "SELECT * FROM " + quotedName);

        const MysqlResPtr res(mysql_use_result(conn));
        if (!res) {
            throw std::runtime_error(mysql_error(conn));
        }

        const unsigned int fieldCount = mysql_num_fields(res.get());
        const std::string insertPrefix = "INSERT INTO " + quotedName + " VALUES ";

        std::string batch;
        std::uint64_t rows = 0;

        const auto flush = [&]() {
            if (batch.empty()) {
                return;
            }
            out << insertPrefix << batch << ";\n";
            batch.clear();
        };

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get())) != nullptr) {
            const unsigned long* lengths = mysql_fetch_lengths(res.get());

            std::string tuple = "(";
            for (unsigned int i = 0; i < fieldCount; ++i) {
                if (i > 0) {
                    tuple += ',';
                }
                tuple += quoteValue(conn, row[i], lengths ? lengths[i] : 0);
            }
            tuple += ')';

            if (!batch.empty() && batch.size() + tuple.size() + 1 > kMaxInsertBytes) {
                flush();
            }
            if (!batch.empty()) {
                batch += ',';
            }
            batch += tuple;

            ++rows;
            progress.rowsWritten.fetch_add(1, std::memory_order_relaxed);

            if ((rows % 1000) == 0) {
                progress.bytesWritten.store(static_cast<std::uint64_t>(out.tellp()),
                                            std::memory_order_relaxed);
                if (cancelled(progress, stopToken)) {
                    stopped = true;
                    break;
                }
            }
        }
        flush();

        // A cancelled mysql_use_result must still be drained before the connection
        // can be reused; ~MysqlResPtr does that via mysql_free_result.
        return rows;
    }

} // namespace

namespace DatabaseExporter {

    std::string promptForSqlDumpDestination(const std::string& defaultName) {
        constexpr nfdfilteritem_t filterItem[1] = {{"SQL Dump", "sql"}};
        nfdchar_t* outPath = nullptr;
        const nfdresult_t dialog = NFD_SaveDialog(&outPath, filterItem, 1, nullptr,
                                                  defaultName.empty() ? nullptr
                                                                      : defaultName.c_str());
        if (dialog != NFD_OKAY) {
            if (dialog == NFD_ERROR) {
                spdlog::error("File dialog error: {}", NFD_GetError());
            }
            return {};
        }
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }

    Result runSqlDump(MySQLDatabaseNode* node, const std::string& path, Progress& progress,
                      const std::stop_token& stopToken) {
        Result result;
        result.path = path;

        if (!node) {
            result.error = "No database selected.";
            return result;
        }

        std::optional<ConnectionPool<MYSQL*>::Session> session;
        try {
            session.emplace(node->getSession());
        } catch (const std::exception& e) {
            result.error = e.what();
            return result;
        }
        MYSQL* conn = session->get();

        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) {
            result.error = "Could not open '" + path + "' for writing.";
            return result;
        }

        std::vector<std::string> tables;
        std::vector<std::string> views;
        bool stopped = false;
        try {
            listObjects(conn, tables, views);
            progress.objectsTotal.store(static_cast<int>(tables.size() + views.size()),
                                        std::memory_order_relaxed);

            out << "-- DearSQL dump of database " << node->name << "\n\n" << kPreamble;

            for (const auto& table : tables) {
                if (cancelled(progress, stopToken)) {
                    stopped = true;
                    break;
                }

                out << "DROP TABLE IF EXISTS " << quoteIdentifier(table) << ";\n";
                out << showCreate(conn, "SHOW CREATE TABLE " + quoteIdentifier(table)) << ";\n\n";

                result.rows += writeTableData(conn, out, table, progress, stopToken, stopped);
                out << "\n";

                ++result.objects;
                progress.objectsDone.store(result.objects, std::memory_order_relaxed);
                progress.bytesWritten.store(static_cast<std::uint64_t>(out.tellp()),
                                            std::memory_order_relaxed);
                if (stopped) {
                    break;
                }
            }

            // Views come after every table so their referenced tables already
            // exist. A view built on another view still depends on declaration
            // order, which this does not attempt to resolve.
            if (!stopped) {
                for (const auto& view : views) {
                    if (cancelled(progress, stopToken)) {
                        stopped = true;
                        break;
                    }

                    out << "DROP VIEW IF EXISTS " << quoteIdentifier(view) << ";\n";
                    out << showCreate(conn, "SHOW CREATE VIEW " + quoteIdentifier(view)) << ";\n\n";

                    ++result.objects;
                    progress.objectsDone.store(result.objects, std::memory_order_relaxed);
                }
            }

            if (!stopped) {
                out << kEpilogue;
            }
            out.flush();
        } catch (const std::exception& e) {
            result.error = e.what();
            spdlog::error("SQL export failed after {} objects: {}", result.objects, e.what());
            return result;
        }

        if (!out) {
            result.error = "Failed writing to '" + path + "'.";
            return result;
        }

        progress.bytesWritten.store(static_cast<std::uint64_t>(out.tellp()),
                                    std::memory_order_relaxed);
        result.cancelled = stopped;
        result.success = !stopped;

        if (stopped) {
            spdlog::warn("SQL export cancelled after {} objects - {}", result.objects, path);
        } else {
            spdlog::info("SQL export complete: {} objects, {} rows - {}", result.objects,
                         result.rows, path);
        }
        return result;
    }

} // namespace DatabaseExporter
