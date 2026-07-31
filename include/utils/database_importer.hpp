#pragma once

#include "database/mysql/mysql_database_node.hpp"

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <string>

namespace DatabaseImporter {

    // Written by the import worker, read by the UI thread each frame.
    //
    // cancelRequested goes the other way: the UI sets it and the worker stops at
    // the next statement boundary. AsyncOperation::cancel() is deliberately not
    // used for this -- it clears the running flag, so check() would never deliver
    // the result and the caller could not report what was applied.
    struct Progress {
        std::atomic<std::uint64_t> bytesRead{0};
        std::atomic<std::uint64_t> totalBytes{0};
        std::atomic<int> statementsApplied{0};
        std::atomic<bool> cancelRequested{false};
    };

    struct Result {
        bool success = false;
        bool cancelled = false;
        int applied = 0;
        std::string error;
        std::string path;
    };

    // Opens a native file picker. Must be called on the UI thread. Returns an
    // empty string if the user cancelled.
    std::string promptForSqlDump();

    // Runs a dump against the node. Safe to call from a worker thread: it holds
    // one pooled connection for the whole run and never touches the UI.
    //
    // Holding a single connection is required, not an optimisation. mysqldump
    // opens with session-scoped state -- FOREIGN_KEY_CHECKS=0, SQL_MODE,
    // SET NAMES -- and closes by restoring it from @OLD_* variables. Taking a
    // fresh connection per statement would let foreign key checks come back on
    // mid-dump and leave the trailing restores reading NULL variables.
    //
    // Statements are concatenated and sent in batches to keep round trips down on
    // large dumps, so `applied` counts statements in batches known to have
    // succeeded; on failure the offending statement is somewhere in the batch
    // after that, not necessarily the next one.
    //
    // Execution stops at the first failing batch -- continuing past a failed
    // CREATE TABLE only produces cascading errors -- and also on cancellation.
    Result runSqlDump(MySQLDatabaseNode* node, const std::string& path, Progress& progress,
                      const std::stop_token& stopToken);

} // namespace DatabaseImporter
