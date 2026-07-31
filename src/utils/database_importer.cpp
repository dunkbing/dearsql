#include "utils/database_importer.hpp"

#include "utils/sql_dump_splitter.hpp"

#include <filesystem>
#include <fstream>
#include <nfd.h>
#include <spdlog/spdlog.h>

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

    Result runSqlDump(IDatabaseNode* node, const std::string& path, Progress& progress,
                      const std::stop_token& stopToken) {
        Result result;
        result.path = path;

        if (!node) {
            result.error = "No database selected.";
            return result;
        }

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

        SqlDumpSplitter::split(file, [&](const std::string& statement) {
            if (stopToken.stop_requested() ||
                progress.cancelRequested.load(std::memory_order_relaxed)) {
                cancelled = true;
                return false;
            }

            const auto queryResult = node->executeQuery(statement);
            if (!queryResult.success()) {
                failure = queryResult.errorMessage();
                return false;
            }

            ++applied;
            progress.statementsApplied.store(applied, std::memory_order_relaxed);

            // Cheap next to a per-statement round trip, and sampling less often
            // leaves the bar short of the end when the dump finishes mid-interval.
            if (const auto pos = file.tellg(); pos >= 0) {
                progress.bytesRead.store(static_cast<std::uint64_t>(pos),
                                         std::memory_order_relaxed);
            }
            return true;
        });

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
