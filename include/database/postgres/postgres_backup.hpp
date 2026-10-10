#pragma once

#include "database/db_interface.hpp"

#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

enum class PostgresBackupFormat { Custom, PlainSql };

struct PostgresToolResult {
    bool success = false;
    int exitCode = -1;
    std::string message;
    std::string output;
};

struct PostgresBackupOptions {
    DatabaseConnectionInfo connectionInfo;
    std::string databaseName;
    std::filesystem::path outputPath;
    PostgresBackupFormat format = PostgresBackupFormat::Custom;
    bool includeCreateDatabase = false;
    bool noOwner = false;
};

struct PostgresRestoreOptions {
    DatabaseConnectionInfo connectionInfo;
    std::string databaseName;
    std::filesystem::path inputPath;
    bool cleanBeforeRestore = false;
    bool ifExists = true;
    bool createDatabase = false;
};

class PostgresBackupService {
public:
    static PostgresToolResult checkToolsAvailable(const std::vector<std::string>& toolNames);
    static PostgresToolResult backupDatabase(const PostgresBackupOptions& options,
                                             std::stop_token stop = {});
    static PostgresToolResult restoreDatabase(const PostgresRestoreOptions& options,
                                              std::stop_token stop = {});
};
