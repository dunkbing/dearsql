#include "utils/database_importer.hpp"

#include "utils/sql_dump_splitter.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <nfd.h>
#include <optional>
#include <spdlog/spdlog.h>

namespace {
    // Statements are concatenated and sent in one round trip; the connection
    // sets CLIENT_MULTI_STATEMENTS, so the server splits them. Kept well under
    // the usual max_allowed_packet so batching cannot make a valid dump
    // unreadable.
    constexpr std::size_t kMaxBatchBytes = 1024 * 1024;
} // namespace

namespace DatabaseImporter {

    std::string promptForSqlDump() {
        constexpr nfdfilteritem_t filterItem[2] = {{"SQL Dump", "sql"}, {"All Files", "*"}};
        nfdchar_t* outPath = nullptr;
        const nfdresult_t dialog = NFD_OpenDialog(&outPath, filterItem, 2, nullptr);
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

        // Held for the whole dump so session state set by its preamble survives.
        // getSession() throws if the pool cannot hand one out.
        std::optional<ConnectionPool<MYSQL*>::Session> session;
        try {
            session.emplace(node->getSession());
        } catch (const std::exception& e) {
            result.error = e.what();
            return result;
        }

        // A dump's LOCK TABLES / UNLOCK TABLES pairs and its preamble
        // (FOREIGN_KEY_CHECKS=0, SQL_MODE) apply to the session, not to a statement.
        // Stopping between a lock and its unlock hands the pool a connection still
        // holding a write lock, which blocks every later reader of that table
        // indefinitely, and still skipping foreign key checks, which silently weakens
        // whatever runs on it next. Declared after the session so it resets before
        // the release.
        const struct SessionReset {
            MYSQL* conn;
            std::string database;

            ~SessionReset() {
                if (mysql_reset_connection(conn) != 0) {
                    spdlog::warn("Could not reset the import session: {}", mysql_error(conn));
                    return;
                }
                // A reset keeps the default database in practice, but that is not
                // documented and the pool hands these connections out per database.
                if (mysql_select_db(conn, database.c_str()) != 0) {
                    spdlog::warn("Could not reselect '{}' after the import: {}", database,
                                 mysql_error(conn));
                }
            }
        } sessionReset{session->get(), node->name};

        std::error_code sizeError;
        const auto size = std::filesystem::file_size(path, sizeError);
        progress.totalBytes.store(sizeError ? 0 : size, std::memory_order_relaxed);
        progress.bytesRead.store(0, std::memory_order_relaxed);
        progress.statementsApplied.store(0, std::memory_order_relaxed);

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            result.error = std::string("Could not open '") + path + "'.";
            return result;
        }

        int applied = 0;
        std::string failure;
        bool cancelled = false;

        std::string batch;
        int batched = 0;

        // Sends the accumulated batch as a single query. Returns false to stop the
        // scan. On failure `applied` is left at the last statement known to have
        // succeeded, so the reported count never overstates what was written.
        const auto flush = [&]() {
            if (batch.empty()) {
                return true;
            }
            const auto queryResult = node->executeQueryOn(session->get(), batch);
            batch.clear();
            if (!queryResult.success()) {
                failure = queryResult.errorMessage();
                batched = 0;
                return false;
            }
            applied += batched;
            batched = 0;
            progress.statementsApplied.store(applied, std::memory_order_relaxed);

            if (const auto pos = file.tellg(); pos >= 0) {
                progress.bytesRead.store(static_cast<std::uint64_t>(pos),
                                         std::memory_order_relaxed);
            }
            return true;
        };

        SqlDumpSplitter::split(file, [&](const std::string& statement, const bool compound) {
            if (stopToken.stop_requested() ||
                progress.cancelRequested.load(std::memory_order_relaxed)) {
                if (!flush()) {
                    return false;
                }
                cancelled = true;
                return false;
            }

            // A trigger or routine body carries internal semicolons, so it goes to
            // the server alone rather than concatenated behind another statement.
            if (compound) {
                if (!flush()) {
                    return false;
                }
                batch = statement;
                batched = 1;
                return flush();
            }

            if (!batch.empty() && batch.size() + statement.size() + 1 > kMaxBatchBytes) {
                if (!flush()) {
                    return false;
                }
            }

            if (!batch.empty()) {
                batch += ';';
            }
            batch += statement;
            ++batched;
            return true;
        });

        if (failure.empty() && !cancelled) {
            flush();
        }

        result.applied = applied;
        result.cancelled = cancelled;
        result.error = failure;
        result.success = failure.empty() && !cancelled;

        // tellg() returns -1 once the stream hits eof, so the last statements of a
        // dump never move the counter. Settle it on the true total when the whole
        // file was consumed, otherwise the bar reports short of where it stopped.
        if (result.success) {
            progress.bytesRead.store(progress.totalBytes.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
        }

        if (cancelled) {
            spdlog::warn("SQL import cancelled after {} statements - {}", applied, path);
        } else if (!failure.empty()) {
            spdlog::error("SQL import aborted after {} statements: {}", applied, failure);
        } else {
            spdlog::info("SQL import complete: {} statements - {}", applied, path);
        }
        return result;
    }

} // namespace DatabaseImporter
