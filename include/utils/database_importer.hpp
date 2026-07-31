#pragma once

#include "database/database_node.hpp"

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

    // Runs a dump against the node. Safe to call from a worker thread: each
    // executeQuery takes its own connection from the pool. Execution stops at the
    // first failing statement -- continuing past a failed CREATE TABLE only
    // produces cascading errors -- and also on cancellation.
    Result runSqlDump(IDatabaseNode* node, const std::string& path, Progress& progress,
                      const std::stop_token& stopToken);

} // namespace DatabaseImporter
