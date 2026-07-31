#include "utils/database_exporter.hpp"

#include "database/mysql/mysql_internal.hpp"
#include "database/sql_builder.hpp"

#include <cstddef>
#include <fstream>
#include <nfd.h>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <vector>

namespace {

    using mysql_internal::MysqlResPtr;

    // Extended INSERTs are capped here, under the usual max_allowed_packet, so a
    // dump this writes can always be read back.
    constexpr std::size_t kMaxInsertBytes = 1024 * 1024;

    // What mysqldump emits. FOREIGN_KEY_CHECKS=0 is the load-bearing one: without
    // it the tables restore in the order written and the constraints reject them.
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

    // Escaping goes through the server connection so it honours the connection
    // charset and is binary safe. Appends in place: this runs once per field, so
    // a temporary here costs two allocations per field over a whole table.
    void appendQuotedValue(MYSQL* conn, std::string& out, const char* data,
                           const unsigned long length) {
        if (!data) {
            out += "NULL";
            return;
        }
        const size_t start = out.size();
        out.resize(start + 2 + length * 2);
        out[start] = '\'';
        const unsigned long written =
            mysql_real_escape_string(conn, out.data() + start + 1, data, length);
        // Escaping reports failure as (unsigned long)-1. Unchecked, the resize
        // below wraps modulo 2^64 back to start and silently writes a truncated,
        // unparseable dump.
        if (written == static_cast<unsigned long>(-1)) {
            throw std::runtime_error("Failed to escape a value for export");
        }
        out.resize(start + 1 + written);
        out += '\'';
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

    // mysql_use_result rather than mysql_store_result: the latter buffers the
    // whole table in client memory. The trade-off is that no other query can run
    // until the result is drained, which is fine -- the export owns the session.
    std::uint64_t writeTableData(MYSQL* conn, const ISQLBuilder& builder, std::ofstream& out,
                                 const std::string& tableName,
                                 DatabaseExporter::Progress& progress,
                                 const std::stop_token& stopToken, bool& stopped) {
        const std::string quotedName = builder.quoteIdentifier(tableName);
        execute(conn, "SELECT * FROM " + quotedName);

        // ~MysqlResPtr drains any unread rows, which a cancelled run relies on.
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

        // Reused across rows so its capacity survives; a fresh string here would be
        // an allocation per row.
        std::string tuple;

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get())) != nullptr) {
            const unsigned long* lengths = mysql_fetch_lengths(res.get());

            tuple.clear();
            tuple += '(';
            for (unsigned int i = 0; i < fieldCount; ++i) {
                if (i > 0) {
                    tuple += ',';
                }
                appendQuotedValue(conn, tuple, row[i], lengths ? lengths[i] : 0);
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

        const auto builder = createSQLBuilder(node->getDatabaseType());

        std::vector<std::string> tables;
        std::vector<std::string> views;
        bool stopped = false;
        try {
            // mysql_real_escape_string escapes according to the *source* server's
            // sql_mode, but kPreamble assigns SQL_MODE on the restoring session.
            // If the source has NO_BACKSLASH_ESCAPES, backslashes are written
            // through unescaped and then re-read as escapes on import, which
            // corrupts the dump and lets table data break out of its string
            // literal. mysqldump avoids this by pinning sql_mode on the dumping
            // connection too; match it, and match what kPreamble declares.
            execute(conn, "SET SESSION sql_mode = 'NO_AUTO_VALUE_ON_ZERO'");

            listObjects(conn, tables, views);
            progress.objectsTotal.store(static_cast<int>(tables.size() + views.size()),
                                        std::memory_order_relaxed);

            out << "-- DearSQL dump of database " << node->name << "\n\n" << kPreamble;

            for (const auto& table : tables) {
                if (cancelled(progress, stopToken)) {
                    stopped = true;
                    break;
                }

                out << "DROP TABLE IF EXISTS " << builder->quoteIdentifier(table) << ";\n";
                out << showCreate(conn, "SHOW CREATE TABLE " + builder->quoteIdentifier(table))
                    << ";\n\n";

                result.rows +=
                    writeTableData(conn, *builder, out, table, progress, stopToken, stopped);
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

                    out << "DROP VIEW IF EXISTS " << builder->quoteIdentifier(view) << ";\n";
                    out << showCreate(conn, "SHOW CREATE VIEW " + builder->quoteIdentifier(view))
                        << ";\n\n";

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
