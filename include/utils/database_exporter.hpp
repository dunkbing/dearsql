#pragma once

#include "database/mysql/mysql_database_node.hpp"

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <string>

namespace DatabaseExporter {

    // Written by the export worker, read by the UI thread each frame.
    // cancelRequested goes the other way; see DatabaseImporter::Progress for why
    // AsyncOperation::cancel() is not used for that.
    struct Progress {
        std::atomic<int> objectsDone{0};
        std::atomic<int> objectsTotal{0};
        std::atomic<std::uint64_t> rowsWritten{0};
        std::atomic<std::uint64_t> bytesWritten{0};
        std::atomic<bool> cancelRequested{false};
    };

    struct Result {
        bool success = false;
        bool cancelled = false;
        int objects = 0;
        std::uint64_t rows = 0;
        std::string error;
        std::string path;
    };

    // Opens a native save dialog. Must be called on the UI thread. Returns an
    // empty string if the user cancelled.
    std::string promptForSqlDumpDestination(const std::string& defaultName);

    // Writes a mysqldump-compatible .sql dump of the whole database.
    //
    // Schema comes from SHOW CREATE TABLE/VIEW, so indexes, foreign keys,
    // defaults, partitioning and AUTO_INCREMENT survive.
    //
    // Safe to call from a worker thread: it holds one pooled connection for the
    // whole run and never touches the UI.
    Result runSqlDump(MySQLDatabaseNode* node, const std::string& path, Progress& progress,
                      const std::stop_token& stopToken);

} // namespace DatabaseExporter
