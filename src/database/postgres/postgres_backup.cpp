#include "database/postgres/postgres_backup.hpp"

#include "utils/process_runner.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <unordered_map>

namespace {
    std::string sslModeEnvValue(const SslMode mode) {
        return sslModeToString(mode);
    }

    std::unordered_map<std::string, std::string>
    postgresEnvironment(const DatabaseConnectionInfo& info) {
        std::unordered_map<std::string, std::string> env;
        if (!info.password.empty()) {
            env["PGPASSWORD"] = info.password;
        }
        env["PGSSLMODE"] = sslModeEnvValue(info.sslmode);
        if ((info.sslmode == SslMode::VerifyCA || info.sslmode == SslMode::VerifyFull) &&
            !info.sslCACertPath.empty()) {
            env["PGSSLROOTCERT"] = info.sslCACertPath;
        }
        return env;
    }

    void appendConnectionArgs(std::vector<std::string>& args, const DatabaseConnectionInfo& info,
                              const std::string& databaseName) {
        args.emplace_back("--host");
        args.emplace_back(info.host);
        args.emplace_back("--port");
        args.emplace_back(std::to_string(info.port));
        if (!info.username.empty()) {
            args.emplace_back("--username");
            args.emplace_back(info.username);
        }
        args.emplace_back("--dbname");
        args.emplace_back(databaseName);
    }

    bool hasSqlExtension(std::filesystem::path path) {
        auto ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".sql";
    }

    std::string maintenanceDatabaseFor(const DatabaseConnectionInfo& info,
                                       const std::string& selectedDatabase) {
        if (info.type == DatabaseType::REDSHIFT) {
            return "dev";
        }
        return selectedDatabase == "postgres" ? "template1" : "postgres";
    }

    PostgresToolResult fromProcessResult(const ProcessResult& process,
                                         const std::string& successMessage) {
        PostgresToolResult result;
        result.success = process.success;
        result.exitCode = process.exitCode;
        result.output = process.output;
        if (process.success) {
            result.message = successMessage;
        } else if (!process.errorMessage.empty()) {
            result.message = process.errorMessage;
        } else {
            result.message =
                std::format("PostgreSQL tool failed with exit code {}", process.exitCode);
        }
        return result;
    }

    std::string firstLine(std::string text) {
        const auto newline = text.find_first_of("\r\n");
        if (newline != std::string::npos) {
            text = text.substr(0, newline);
        }
        return text;
    }
} // namespace

PostgresToolResult
PostgresBackupService::checkToolsAvailable(const std::vector<std::string>& toolNames) {
    std::ostringstream versions;
    for (const auto& toolName : toolNames) {
        ProcessSpec spec{{toolName, "--version"}, {}, std::chrono::seconds(5)};
        ProcessResult process = ProcessRunner::run(spec);
        if (!process.success) {
            PostgresToolResult result;
            result.success = false;
            result.exitCode = process.exitCode;
            result.output = process.output;
            result.message = std::format(
                "PostgreSQL client tool '{}' was not found or failed to run.\n\n"
                "Install PostgreSQL client tools and make sure '{}' is available in PATH.",
                toolName, toolName);
            return result;
        }

        if (!process.output.empty()) {
            versions << firstLine(process.output) << '\n';
        }
    }

    PostgresToolResult result;
    result.success = true;
    result.message = versions.str();
    return result;
}

PostgresToolResult PostgresBackupService::backupDatabase(const PostgresBackupOptions& options,
                                                         std::stop_token stop) {
    std::vector<std::string> args{"pg_dump"};
    appendConnectionArgs(args, options.connectionInfo, options.databaseName);

    args.emplace_back("--file");
    args.emplace_back(options.outputPath.string());
    args.emplace_back("--format");
    args.emplace_back(options.format == PostgresBackupFormat::Custom ? "custom" : "plain");

    if (options.includeCreateDatabase) {
        args.emplace_back("--create");
    }
    if (options.noOwner) {
        args.emplace_back("--no-owner");
    }
    args.emplace_back("--verbose");

    ProcessSpec spec{std::move(args), postgresEnvironment(options.connectionInfo)};
    return fromProcessResult(ProcessRunner::run(spec, stop),
                             std::format("Backup completed: {}", options.outputPath.string()));
}

PostgresToolResult PostgresBackupService::restoreDatabase(const PostgresRestoreOptions& options,
                                                          std::stop_token stop) {
    const bool plainSql = hasSqlExtension(options.inputPath);
    const std::string targetDatabase =
        options.createDatabase
            ? maintenanceDatabaseFor(options.connectionInfo, options.databaseName)
            : options.databaseName;

    std::vector<std::string> args;
    if (plainSql) {
        args.emplace_back("psql");
        appendConnectionArgs(args, options.connectionInfo, targetDatabase);
        args.emplace_back("--set");
        args.emplace_back("ON_ERROR_STOP=1");
        args.emplace_back("--file");
        args.emplace_back(options.inputPath.string());
    } else {
        args.emplace_back("pg_restore");
        appendConnectionArgs(args, options.connectionInfo, targetDatabase);
        if (options.cleanBeforeRestore) {
            args.emplace_back("--clean");
            if (options.ifExists) {
                args.emplace_back("--if-exists");
            }
        }
        if (options.createDatabase) {
            args.emplace_back("--create");
        }
        args.emplace_back("--verbose");
        args.emplace_back(options.inputPath.string());
    }

    ProcessSpec spec{std::move(args), postgresEnvironment(options.connectionInfo)};
    return fromProcessResult(ProcessRunner::run(spec, stop),
                             std::format("Restore completed from: {}", options.inputPath.string()));
}
