#include "app_state.hpp"
#include "utils/crypto.hpp"
#include "utils/logger.hpp"
#include <filesystem>
#include <format>
#include <iostream>
#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

    // RAII wrapper for sqlite3_stmt
    struct StmtDeleter {
        void operator()(sqlite3_stmt* stmt) const {
            if (stmt)
                sqlite3_finalize(stmt);
        }
    };
    using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

    // Return column text as string, or "NULL" if the column is null
    std::string columnText(sqlite3_stmt* stmt, int col) {
        if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
            return "NULL";
        }
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return text ? std::string(text) : "NULL";
    }

    // Parse a row from the saved_connections query into a SavedConnection
    bool parseConnectionRow(sqlite3_stmt* stmt, SavedConnection& conn) {
        conn.id = sqlite3_column_int(stmt, 0);
        conn.connectionInfo.name = columnText(stmt, 1);

        std::string typeStr = columnText(stmt, 2);
        if (typeStr == "sqlite") {
            conn.connectionInfo.type = DatabaseType::SQLITE;
        } else if (typeStr == "postgresql") {
            conn.connectionInfo.type = DatabaseType::POSTGRESQL;
        } else if (typeStr == "mysql") {
            conn.connectionInfo.type = DatabaseType::MYSQL;
        } else if (typeStr == "redis") {
            conn.connectionInfo.type = DatabaseType::REDIS;
        } else if (typeStr == "mongodb") {
            conn.connectionInfo.type = DatabaseType::MONGODB;
        } else if (typeStr == "mariadb") {
            conn.connectionInfo.type = DatabaseType::MARIADB;
        } else {
            Logger::warn(std::format("Unknown database type '{}' for connection '{}', skipping",
                                     typeStr, conn.connectionInfo.name));
            return false;
        }

        std::string hostStr = columnText(stmt, 3);
        conn.connectionInfo.host = (hostStr == "NULL") ? "" : hostStr;

        std::string portStr = columnText(stmt, 4);
        conn.connectionInfo.port = (portStr == "NULL" || portStr.empty()) ? 0 : std::stoi(portStr);

        std::string dbStr = columnText(stmt, 5);
        conn.connectionInfo.database = (dbStr == "NULL") ? "" : dbStr;

        std::string encryptedUsername = columnText(stmt, 6);
        if (encryptedUsername == "NULL")
            encryptedUsername = "";

        std::string encryptedPassword = columnText(stmt, 7);
        if (encryptedPassword == "NULL")
            encryptedPassword = "";

        std::string saltStr = columnText(stmt, 9);
        if (saltStr == "NULL")
            saltStr = "";

        // Derive encryption key once for both DB and SSH credentials
        std::string encryptionKey;
        try {
            if (!saltStr.empty()) {
                auto saltData = CryptoUtils::base64Decode(saltStr);
                std::string salt(saltData.begin(), saltData.end());
                encryptionKey = CryptoUtils::deriveKey("dearsql-master-key", salt);

                if (!encryptedUsername.empty()) {
                    try {
                        conn.connectionInfo.username =
                            CryptoUtils::decrypt(encryptedUsername, encryptionKey);
                    } catch (...) {
                        conn.connectionInfo.username = "";
                    }
                }

                if (!encryptedPassword.empty()) {
                    try {
                        conn.connectionInfo.password =
                            CryptoUtils::decrypt(encryptedPassword, encryptionKey);
                    } catch (...) {
                        conn.connectionInfo.password = "";
                    }
                }
            } else {
                conn.connectionInfo.username = encryptedUsername;
                conn.connectionInfo.password = encryptedPassword;
            }
        } catch (...) {
            conn.connectionInfo.username = "";
            conn.connectionInfo.password = "";
        }

        std::string pathStr = columnText(stmt, 8);
        conn.connectionInfo.path = (pathStr == "NULL") ? "" : pathStr;

        conn.lastUsed = columnText(stmt, 10);
        if (conn.lastUsed == "NULL")
            conn.lastUsed = "";

        std::string workspaceIdStr = columnText(stmt, 11);
        conn.workspaceId =
            (workspaceIdStr == "NULL" || workspaceIdStr.empty()) ? 1 : std::stoi(workspaceIdStr);

        std::string showAllStr = columnText(stmt, 12);
        conn.connectionInfo.showAllDatabases =
            (showAllStr != "NULL" && showAllStr != "0" && !showAllStr.empty());

        std::string sslmodeStr = columnText(stmt, 13);
        conn.connectionInfo.sslmode = (sslmodeStr == "NULL" || sslmodeStr.empty())
                                          ? SslMode::Prefer
                                          : stringToSslMode(sslmodeStr);

        // SSH tunnel fields (columns 14-20)
        std::string sshEnabledStr = columnText(stmt, 14);
        conn.connectionInfo.ssh.enabled =
            (sshEnabledStr != "NULL" && sshEnabledStr != "0" && !sshEnabledStr.empty());

        std::string sshHostStr = columnText(stmt, 15);
        conn.connectionInfo.ssh.host = (sshHostStr == "NULL") ? "" : sshHostStr;

        std::string sshPortStr = columnText(stmt, 16);
        conn.connectionInfo.ssh.port =
            (sshPortStr == "NULL" || sshPortStr.empty()) ? 22 : std::stoi(sshPortStr);

        std::string encryptedSshUsername = columnText(stmt, 17);
        if (encryptedSshUsername == "NULL")
            encryptedSshUsername = "";

        std::string sshAuthStr = columnText(stmt, 18);
        conn.connectionInfo.ssh.authMethod =
            (sshAuthStr == "privatekey") ? SSHAuthMethod::PrivateKey : SSHAuthMethod::Password;

        std::string sshKeyPath = columnText(stmt, 19);
        conn.connectionInfo.ssh.privateKeyPath = (sshKeyPath == "NULL") ? "" : sshKeyPath;

        std::string encryptedSshPassword = columnText(stmt, 20);
        if (encryptedSshPassword == "NULL")
            encryptedSshPassword = "";

        std::string sslCACertPath = columnText(stmt, 21);
        conn.connectionInfo.sslCACertPath = (sslCACertPath == "NULL") ? "" : sslCACertPath;

        // Decrypt SSH credentials reusing the same per-row key
        if (!saltStr.empty() && !encryptionKey.empty()) {
            if (!encryptedSshUsername.empty()) {
                try {
                    conn.connectionInfo.ssh.username =
                        CryptoUtils::decrypt(encryptedSshUsername, encryptionKey);
                } catch (...) {
                    conn.connectionInfo.ssh.username = "";
                }
            }
            if (!encryptedSshPassword.empty()) {
                try {
                    conn.connectionInfo.ssh.password =
                        CryptoUtils::decrypt(encryptedSshPassword, encryptionKey);
                } catch (...) {
                    conn.connectionInfo.ssh.password = "";
                }
            }
        } else {
            conn.connectionInfo.ssh.username = encryptedSshUsername;
            conn.connectionInfo.ssh.password = encryptedSshPassword;
        }

        return true;
    }

} // namespace

AppState::AppState() {
    fs::path dbPath_;

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else // Assume Unix-like
    const char* home = std::getenv("HOME");
#endif

    if (home) {
        dbPath_ = fs::path(home) / ".dearsql" / "connections.db";
    } else {
        dbPath_ = fs::path("./connections.db");
    }

    dbPath = dbPath_.string();
    std::cout << dbPath << "\n";
}

AppState::~AppState() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool AppState::initialize() {
    // Create directory if it doesn't exist
    if (const fs::path dir = fs::path(dbPath).parent_path(); !fs::exists(dir)) {
        fs::create_directories(dir);
    }

    int rc = sqlite3_open_v2(dbPath.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open app state database: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // limit page cache to ~1MB (state DB is tiny)
    sqlite3_exec(db_, "PRAGMA cache_size = -1000;", nullptr, nullptr, nullptr);

    return createTables();
}

bool AppState::createTables() {
    const std::string createConnectionsTable = R"(
        CREATE TABLE IF NOT EXISTS saved_connections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            type TEXT NOT NULL,
            host TEXT,
            port INTEGER,
            database_name TEXT,
            username TEXT,
            password TEXT,
            path TEXT,
            salt TEXT,
            last_used DATETIME DEFAULT CURRENT_TIMESTAMP,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            workspace_id INTEGER DEFAULT 1,
            show_all_databases INTEGER DEFAULT 0
        );
    )";

    const std::string createSettingsTable = R"(
        CREATE TABLE IF NOT EXISTS app_settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createWorkspacesTable = R"(
        CREATE TABLE IF NOT EXISTS workspaces (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            description TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            last_used DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const bool success = executeSQL(createConnectionsTable) && executeSQL(createSettingsTable) &&
                         executeSQL(createWorkspacesTable);

    auto ensureColumnExists = [this](const std::string& columnName, const std::string& alterSql) {
        try {
            const std::string checkSql =
                "SELECT COUNT(*) FROM pragma_table_info('saved_connections') WHERE name = ?";
            sqlite3_stmt* raw = nullptr;
            const int rc = sqlite3_prepare_v2(db_, checkSql.c_str(), -1, &raw, nullptr);
            if (rc != SQLITE_OK)
                return;
            const StmtPtr stmt(raw);
            sqlite3_bind_text(stmt.get(), 1, columnName.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                const int count = sqlite3_column_int(stmt.get(), 0);
                if (count == 0) {
                    executeSQL(alterSql);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to check column existence: " << e.what() << std::endl;
        }
    };

    ensureColumnExists("workspace_id",
                       "ALTER TABLE saved_connections ADD COLUMN workspace_id INTEGER DEFAULT 1;");
    ensureColumnExists(
        "show_all_databases",
        "ALTER TABLE saved_connections ADD COLUMN show_all_databases INTEGER DEFAULT 0;");
    ensureColumnExists("sslmode",
                       "ALTER TABLE saved_connections ADD COLUMN sslmode TEXT DEFAULT 'prefer';");

    // SSH tunnel columns
    ensureColumnExists("ssh_enabled",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_enabled INTEGER DEFAULT 0;");
    ensureColumnExists("ssh_host", "ALTER TABLE saved_connections ADD COLUMN ssh_host TEXT;");
    ensureColumnExists("ssh_port",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_port INTEGER DEFAULT 22;");
    ensureColumnExists("ssh_username",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_username TEXT;");
    ensureColumnExists(
        "ssh_auth_method",
        "ALTER TABLE saved_connections ADD COLUMN ssh_auth_method TEXT DEFAULT 'password';");
    ensureColumnExists("ssh_private_key_path",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_private_key_path TEXT;");
    ensureColumnExists("ssh_password",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_password TEXT;");
    ensureColumnExists("ssl_ca_cert_path",
                       "ALTER TABLE saved_connections ADD COLUMN ssl_ca_cert_path TEXT;");

    // Ensure default workspace exists
    if (success) {
        ensureDefaultWorkspace();
    }

    return success;
}

bool AppState::executeSQL(const std::string& sql) const {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << (errmsg ? errmsg : "unknown") << std::endl;
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

int AppState::saveConnection(const SavedConnection& connection) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO saved_connections
        (name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id,
         show_all_databases, sslmode,
         ssh_enabled, ssh_host, ssh_port, ssh_username, ssh_auth_method, ssh_private_key_path, ssh_password,
         ssl_ca_cert_path)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    std::string encryptionKey = CryptoUtils::deriveKey("dearsql-master-key", salt);
    std::string encryptedUsername =
        connection.connectionInfo.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.username, encryptionKey);
    std::string encryptedPassword =
        connection.connectionInfo.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.password, encryptionKey);
    std::string encryptedSshUsername =
        connection.connectionInfo.ssh.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.username, encryptionKey);
    std::string encryptedSshPassword =
        connection.connectionInfo.ssh.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.password, encryptionKey);

    std::string typeStr;
    switch (connection.connectionInfo.type) {
    case DatabaseType::SQLITE:
        typeStr = "sqlite";
        break;
    case DatabaseType::POSTGRESQL:
        typeStr = "postgresql";
        break;
    case DatabaseType::MYSQL:
        typeStr = "mysql";
        break;
    case DatabaseType::REDIS:
        typeStr = "redis";
        break;
    case DatabaseType::MONGODB:
        typeStr = "mongodb";
        break;
    case DatabaseType::MARIADB:
        typeStr = "mariadb";
        break;
    }

    std::string saltBase64 =
        CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()));
    int showAll = connection.connectionInfo.showAllDatabases ? 1 : 0;

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save connection: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    StmtPtr stmt(raw);

    sqlite3_bind_text(stmt.get(), 1, connection.connectionInfo.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, typeStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, connection.connectionInfo.host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, connection.connectionInfo.port);
    sqlite3_bind_text(stmt.get(), 5, connection.connectionInfo.database.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, encryptedUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 7, encryptedPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, connection.connectionInfo.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 9, saltBase64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 10, connection.workspaceId);
    sqlite3_bind_int(stmt.get(), 11, showAll);
    auto sslmodeStr = sslModeToString(connection.connectionInfo.sslmode);
    sqlite3_bind_text(stmt.get(), 12, sslmodeStr.c_str(), -1, SQLITE_TRANSIENT);

    // SSH tunnel fields (params 13-19)
    sqlite3_bind_int(stmt.get(), 13, connection.connectionInfo.ssh.enabled ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 14, connection.connectionInfo.ssh.host.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 15, connection.connectionInfo.ssh.port);
    sqlite3_bind_text(stmt.get(), 16, encryptedSshUsername.c_str(), -1, SQLITE_TRANSIENT);
    std::string sshAuthStr = connection.connectionInfo.ssh.authMethod == SSHAuthMethod::PrivateKey
                                 ? "privatekey"
                                 : "password";
    sqlite3_bind_text(stmt.get(), 17, sshAuthStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 18, connection.connectionInfo.ssh.privateKeyPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 19, encryptedSshPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 20, connection.connectionInfo.sslCACertPath.c_str(), -1,
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save connection: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }

    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

bool AppState::updateConnection(const SavedConnection& connection) const {
    const std::string sql = R"(
        UPDATE saved_connections
        SET name = ?, type = ?, host = ?, port = ?, database_name = ?,
            username = ?, password = ?, path = ?, salt = ?, last_used = CURRENT_TIMESTAMP,
            workspace_id = ?, show_all_databases = ?, sslmode = ?,
            ssh_enabled = ?, ssh_host = ?, ssh_port = ?, ssh_username = ?,
            ssh_auth_method = ?, ssh_private_key_path = ?, ssh_password = ?,
            ssl_ca_cert_path = ?
        WHERE id = ?;
    )";

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    std::string encryptionKey = CryptoUtils::deriveKey("dearsql-master-key", salt);
    std::string encryptedUsername =
        connection.connectionInfo.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.username, encryptionKey);
    std::string encryptedPassword =
        connection.connectionInfo.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.password, encryptionKey);
    std::string encryptedSshUsername =
        connection.connectionInfo.ssh.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.username, encryptionKey);
    std::string encryptedSshPassword =
        connection.connectionInfo.ssh.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.password, encryptionKey);

    std::string typeStr;
    switch (connection.connectionInfo.type) {
    case DatabaseType::SQLITE:
        typeStr = "sqlite";
        break;
    case DatabaseType::POSTGRESQL:
        typeStr = "postgresql";
        break;
    case DatabaseType::MYSQL:
        typeStr = "mysql";
        break;
    case DatabaseType::REDIS:
        typeStr = "redis";
        break;
    case DatabaseType::MONGODB:
        typeStr = "mongodb";
        break;
    case DatabaseType::MARIADB:
        typeStr = "mariadb";
        break;
    }

    std::string saltBase64 =
        CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()));
    int showAll = connection.connectionInfo.showAllDatabases ? 1 : 0;

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);

    sqlite3_bind_text(stmt.get(), 1, connection.connectionInfo.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, typeStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, connection.connectionInfo.host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, connection.connectionInfo.port);
    sqlite3_bind_text(stmt.get(), 5, connection.connectionInfo.database.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, encryptedUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 7, encryptedPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, connection.connectionInfo.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 9, saltBase64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 10, connection.workspaceId);
    sqlite3_bind_int(stmt.get(), 11, showAll);
    auto sslmodeStr = sslModeToString(connection.connectionInfo.sslmode);
    sqlite3_bind_text(stmt.get(), 12, sslmodeStr.c_str(), -1, SQLITE_TRANSIENT);

    // SSH tunnel fields (params 13-19)
    sqlite3_bind_int(stmt.get(), 13, connection.connectionInfo.ssh.enabled ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 14, connection.connectionInfo.ssh.host.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 15, connection.connectionInfo.ssh.port);
    sqlite3_bind_text(stmt.get(), 16, encryptedSshUsername.c_str(), -1, SQLITE_TRANSIENT);
    std::string sshAuthStr = connection.connectionInfo.ssh.authMethod == SSHAuthMethod::PrivateKey
                                 ? "privatekey"
                                 : "password";
    sqlite3_bind_text(stmt.get(), 17, sshAuthStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 18, connection.connectionInfo.ssh.privateKeyPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 19, encryptedSshPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 20, connection.connectionInfo.sslCACertPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 21, connection.id);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::vector<SavedConnection> AppState::getSavedConnections() const {
    Logger::info("AppState::getSavedConnections() - Loading saved connections...");
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used,
               COALESCE(workspace_id, 1) as workspace_id,
               COALESCE(show_all_databases, 0) as show_all_databases,
               COALESCE(sslmode, 'prefer') as sslmode,
               COALESCE(ssh_enabled, 0) as ssh_enabled,
               ssh_host, COALESCE(ssh_port, 22) as ssh_port,
               ssh_username, COALESCE(ssh_auth_method, 'password') as ssh_auth_method,
               ssh_private_key_path, ssh_password,
               COALESCE(ssl_ca_cert_path, '') as ssl_ca_cert_path
        FROM saved_connections
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        Logger::error(std::format("Failed to fetch connections: {}", sqlite3_errmsg(db_)));
        return connections;
    }
    StmtPtr stmt(raw);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SavedConnection conn;
        if (!parseConnectionRow(stmt.get(), conn))
            continue;

        Logger::info(std::format("Loaded connection: id={}, name='{}', type={}, host='{}', port={}",
                                 conn.id, conn.connectionInfo.name,
                                 static_cast<int>(conn.connectionInfo.type),
                                 conn.connectionInfo.host, conn.connectionInfo.port));
        connections.push_back(conn);
    }

    Logger::info(
        std::format("AppState::getSavedConnections() - Loaded {} connections", connections.size()));
    return connections;
}

bool AppState::deleteConnection(const int connectionId) const {
    const std::string sql = "DELETE FROM saved_connections WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to delete connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, connectionId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to delete connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::updateLastUsed(const int connectionId) const {
    const std::string sql =
        "UPDATE saved_connections SET last_used = CURRENT_TIMESTAMP WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update last used: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, connectionId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update last used: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::setSetting(const std::string& key, const std::string& value) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO app_settings (key, value, updated_at)
        VALUES (?, ?, CURRENT_TIMESTAMP);
    )";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save setting: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save setting: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::string AppState::getSetting(const std::string& key, const std::string& defaultValue) const {
    const std::string sql = "SELECT value FROM app_settings WHERE key = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK)
        return defaultValue;
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL) {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            if (text)
                return std::string(text);
        }
    }
    return defaultValue;
}

int AppState::saveWorkspace(const Workspace& workspace) const {
    const std::string sql = R"(
        INSERT INTO workspaces (name, description, last_used)
        VALUES (?, ?, CURRENT_TIMESTAMP);
    )";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save workspace: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, workspace.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, workspace.description.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save workspace: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Workspace> AppState::getWorkspaces() const {
    std::vector<Workspace> workspaces;
    const std::string sql = R"(
        SELECT id, name, description, created_at, last_used
        FROM workspaces
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to fetch workspaces: " << sqlite3_errmsg(db_) << std::endl;
        return workspaces;
    }
    StmtPtr stmt(raw);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Workspace workspace;
        workspace.id = sqlite3_column_int(stmt.get(), 0);

        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        workspace.name = name ? std::string(name) : "";

        std::string desc = columnText(stmt.get(), 2);
        workspace.description = (desc == "NULL") ? "" : desc;

        std::string createdAt = columnText(stmt.get(), 3);
        workspace.createdAt = (createdAt == "NULL") ? "" : createdAt;

        std::string lastUsed = columnText(stmt.get(), 4);
        workspace.lastUsed = (lastUsed == "NULL") ? "" : lastUsed;

        workspaces.push_back(workspace);
    }

    return workspaces;
}

bool AppState::deleteWorkspace(const int workspaceId) const {
    if (workspaceId == 1)
        return false;

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to begin transaction: " << (errmsg ? errmsg : "unknown") << std::endl;
        sqlite3_free(errmsg);
        return false;
    }

    // Move connections to default workspace
    {
        const std::string sql =
            "UPDATE saved_connections SET workspace_id = 1 WHERE workspace_id = ?";
        sqlite3_stmt* raw = nullptr;
        rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        StmtPtr stmt(raw);
        sqlite3_bind_int(stmt.get(), 1, workspaceId);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
    }

    // Delete workspace
    {
        const std::string sql = "DELETE FROM workspaces WHERE id = ?";
        sqlite3_stmt* raw = nullptr;
        rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        StmtPtr stmt(raw);
        sqlite3_bind_int(stmt.get(), 1, workspaceId);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
    }

    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return true;
}

bool AppState::updateWorkspaceLastUsed(const int workspaceId) const {
    const std::string sql = "UPDATE workspaces SET last_used = CURRENT_TIMESTAMP WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update workspace usage: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, workspaceId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update workspace usage: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::vector<SavedConnection> AppState::getConnectionsForWorkspace(const int workspaceId) const {
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id,
               show_all_databases,
               COALESCE(sslmode, 'prefer') as sslmode,
               COALESCE(ssh_enabled, 0) as ssh_enabled,
               ssh_host, COALESCE(ssh_port, 22) as ssh_port,
               ssh_username, COALESCE(ssh_auth_method, 'password') as ssh_auth_method,
               ssh_private_key_path, ssh_password,
               COALESCE(ssl_ca_cert_path, '') as ssl_ca_cert_path
        FROM saved_connections
        WHERE workspace_id = ?
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to fetch workspace connections: " << sqlite3_errmsg(db_) << std::endl;
        return connections;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, workspaceId);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SavedConnection conn;
        if (!parseConnectionRow(stmt.get(), conn))
            continue;
        connections.push_back(conn);
    }

    return connections;
}

bool AppState::moveConnectionToWorkspace(const int connectionId, const int workspaceId) const {
    const std::string sql = "UPDATE saved_connections SET workspace_id = ? WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to move connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, workspaceId);
    sqlite3_bind_int(stmt.get(), 2, connectionId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to move connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::ensureDefaultWorkspace() const {
    // Check if default workspace exists
    {
        const std::string sql = "SELECT COUNT(*) FROM workspaces WHERE id = 1";
        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to ensure default workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        StmtPtr stmt(raw);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt.get(), 0);
            if (count > 0)
                return true;
        }
    }

    const std::string insertSql = R"(
        INSERT INTO workspaces (id, name, description, created_at, last_used)
        VALUES (1, 'Default', 'Default workspace for all connections', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);
    )";
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, insertSql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to ensure default workspace: " << (errmsg ? errmsg : "unknown")
                  << std::endl;
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}
