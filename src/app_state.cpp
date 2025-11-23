#include "app_state.hpp"
#include "database/db.hpp"
#include "utils/crypto.hpp"
#include <filesystem>
#include <iostream>
#include <soci/sqlite3/soci-sqlite3.h>

namespace fs = std::filesystem;

AppState::AppState() {
    fs::path dbPath_;

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else // Assume Unix-like
    const char* home = std::getenv("HOME");
#endif

    if (home) {
        dbPath_ = fs::path(home) / ".dear-sql" / "app_state.db";
    } else {
        dbPath_ = fs::path("./app_state.db");
    }

    dbPath = dbPath_.string();
    std::cout << dbPath << "\n";
}

AppState::~AppState() = default;

bool AppState::initialize() {
    // Create directory if it doesn't exist
    if (const fs::path dir = fs::path(dbPath).parent_path(); !fs::exists(dir)) {
        fs::create_directories(dir);
    }

    try {
        session = std::make_unique<soci::session>(soci::sqlite3, dbPath);
    } catch (const std::exception& e) {
        std::cerr << "Failed to open app state database: " << e.what() << std::endl;
        return false;
    }

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
            int count = 0;
            *session
                << "SELECT COUNT(*) FROM pragma_table_info('saved_connections') WHERE name = :name",
                soci::into(count), soci::use(columnName);

            if (count == 0) {
                executeSQL(alterSql);
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

    // Ensure default workspace exists
    if (success) {
        ensureDefaultWorkspace();
    }

    return success;
}

bool AppState::executeSQL(const std::string& sql) const {
    try {
        *session << sql;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "SQL error: " << e.what() << std::endl;
        return false;
    }
}

int AppState::saveConnection(const SavedConnection& connection) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO saved_connections
        (name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id,
         show_all_databases)
        VALUES (:name, :type, :host, :port, :db, :user, :pass, :path, :salt, CURRENT_TIMESTAMP, :ws, :show);
    )";

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);
    std::string encryptedUsername =
        connection.connectionInfo.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.username, encryptionKey);
    std::string encryptedPassword =
        connection.connectionInfo.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.password, encryptionKey);

    // Convert DatabaseType to string
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
    }

    std::string saltBase64 =
        CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()));
    int showAll = connection.connectionInfo.showAllDatabases ? 1 : 0;

    try {
        *session << sql, soci::use(connection.connectionInfo.name), soci::use(typeStr),
            soci::use(connection.connectionInfo.host), soci::use(connection.connectionInfo.port),
            soci::use(connection.connectionInfo.database), soci::use(encryptedUsername),
            soci::use(encryptedPassword), soci::use(connection.connectionInfo.path),
            soci::use(saltBase64), soci::use(connection.workspaceId), soci::use(showAll);

        long long id = 0;
        *session << "select last_insert_rowid()", soci::into(id);
        return static_cast<int>(id);
    } catch (const std::exception& e) {
        std::cerr << "Failed to save connection: " << e.what() << std::endl;
        return -1;
    }
}

bool AppState::updateConnection(const SavedConnection& connection) const {
    const std::string sql = R"(
        UPDATE saved_connections
        SET name = :name, type = :type, host = :host, port = :port, database_name = :db,
            username = :user, password = :pass, path = :path, salt = :salt, last_used = CURRENT_TIMESTAMP,
            workspace_id = :ws, show_all_databases = :show
        WHERE id = :id;
    )";

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);
    std::string encryptedUsername =
        connection.connectionInfo.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.username, encryptionKey);
    std::string encryptedPassword =
        connection.connectionInfo.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.password, encryptionKey);

    // Convert DatabaseType to string
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
    }

    std::string saltBase64 =
        CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()));
    int showAll = connection.connectionInfo.showAllDatabases ? 1 : 0;

    try {
        *session << sql, soci::use(connection.connectionInfo.name), soci::use(typeStr),
            soci::use(connection.connectionInfo.host), soci::use(connection.connectionInfo.port),
            soci::use(connection.connectionInfo.database), soci::use(encryptedUsername),
            soci::use(encryptedPassword), soci::use(connection.connectionInfo.path),
            soci::use(saltBase64), soci::use(connection.workspaceId), soci::use(showAll),
            soci::use(connection.id);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to update connection: " << e.what() << std::endl;
        return false;
    }
}

std::vector<SavedConnection> AppState::getSavedConnections() const {
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used,
               COALESCE(workspace_id, 1) as workspace_id,
               COALESCE(show_all_databases, 0) as show_all_databases
        FROM saved_connections
        ORDER BY last_used DESC;
    )";

    try {
        soci::rowset<soci::row> rs = (session->prepare << sql);

        for (const auto& row : rs) {
            SavedConnection conn;
            conn.id = row.get<int>(0);
            conn.connectionInfo.name = row.get<std::string>(1);

            // Convert type string to DatabaseType
            std::string typeStr = row.get<std::string>(2);
            if (typeStr == "sqlite") {
                conn.connectionInfo.type = DatabaseType::SQLITE;
            } else if (typeStr == "postgresql") {
                conn.connectionInfo.type = DatabaseType::POSTGRESQL;
            } else if (typeStr == "mysql") {
                conn.connectionInfo.type = DatabaseType::MYSQL;
            } else if (typeStr == "redis") {
                conn.connectionInfo.type = DatabaseType::REDIS;
            }

            conn.connectionInfo.host = row.get<std::string>(3, "");
            conn.connectionInfo.port = row.get<int>(4, 0);
            conn.connectionInfo.database = row.get<std::string>(5, "");

            // Decrypt sensitive data
            std::string encryptedUsername = row.get<std::string>(6, "");
            std::string encryptedPassword = row.get<std::string>(7, "");
            std::string saltStr = row.get<std::string>(9, "");

            try {
                if (!saltStr.empty()) {
                    auto saltData = CryptoUtils::base64Decode(saltStr);
                    std::string salt(saltData.begin(), saltData.end());
                    std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);

                    // Decrypt username
                    if (!encryptedUsername.empty()) {
                        try {
                            conn.connectionInfo.username =
                                CryptoUtils::decrypt(encryptedUsername, encryptionKey);
                        } catch (const std::exception& e) {
                            std::cerr << "Failed to decrypt username for connection "
                                      << conn.connectionInfo.name << ": " << e.what() << std::endl;
                            conn.connectionInfo.username = "";
                        }
                    } else {
                        conn.connectionInfo.username = "";
                    }

                    // Decrypt password
                    if (!encryptedPassword.empty()) {
                        try {
                            conn.connectionInfo.password =
                                CryptoUtils::decrypt(encryptedPassword, encryptionKey);
                        } catch (const std::exception& e) {
                            std::cerr << "Failed to decrypt password for connection "
                                      << conn.connectionInfo.name << ": " << e.what() << std::endl;
                            conn.connectionInfo.password = "";
                        }
                    } else {
                        conn.connectionInfo.password = "";
                    }
                } else {
                    conn.connectionInfo.username = encryptedUsername;
                    conn.connectionInfo.password = encryptedPassword;
                }
            } catch (const std::exception& e) {
                std::cerr << "Failed to process credentials for connection "
                          << conn.connectionInfo.name << ": " << e.what() << std::endl;
                conn.connectionInfo.username = "";
                conn.connectionInfo.password = "";
            }

            conn.connectionInfo.path = row.get<std::string>(8, "");

            conn.lastUsed = convertRowValue(row, 10);
            if (conn.lastUsed == "NULL")
                conn.lastUsed = "";

            conn.workspaceId = row.get<int>(11);
            conn.connectionInfo.showAllDatabases = row.get<int>(12) != 0;

            connections.push_back(conn);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to fetch connections: " << e.what() << std::endl;
    }

    return connections;
}

bool AppState::deleteConnection(const int connectionId) const {
    const std::string sql = "DELETE FROM saved_connections WHERE id = :id";
    try {
        *session << sql, soci::use(connectionId);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to delete connection: " << e.what() << std::endl;
        return false;
    }
}

bool AppState::updateLastUsed(const int connectionId) const {
    const std::string sql =
        "UPDATE saved_connections SET last_used = CURRENT_TIMESTAMP WHERE id = :id";
    try {
        *session << sql, soci::use(connectionId);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to update last used: " << e.what() << std::endl;
        return false;
    }
}

bool AppState::setSetting(const std::string& key, const std::string& value) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO app_settings (key, value, updated_at)
        VALUES (:key, :value, CURRENT_TIMESTAMP);
    )";
    try {
        *session << sql, soci::use(key), soci::use(value);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to save setting: " << e.what() << std::endl;
        return false;
    }
}

std::string AppState::getSetting(const std::string& key, const std::string& defaultValue) const {
    const std::string sql = "SELECT value FROM app_settings WHERE key = :key";
    std::string value;
    soci::indicator ind;
    try {
        *session << sql, soci::use(key), soci::into(value, ind);
        if (session->got_data() && ind != soci::i_null) {
            return value;
        }
    } catch (const std::exception& e) {
        // Just return default if not found or error
    }
    return defaultValue;
}

int AppState::saveWorkspace(const Workspace& workspace) const {
    const std::string sql = R"(
        INSERT INTO workspaces (name, description, last_used)
        VALUES (:name, :desc, CURRENT_TIMESTAMP);
    )";
    try {
        *session << sql, soci::use(workspace.name), soci::use(workspace.description);

        long long id = 0;
        *session << "select last_insert_rowid()", soci::into(id);
        return static_cast<int>(id);
    } catch (const std::exception& e) {
        std::cerr << "Failed to save workspace: " << e.what() << std::endl;
        return -1;
    }
}

std::vector<Workspace> AppState::getWorkspaces() const {
    std::vector<Workspace> workspaces;
    const std::string sql = R"(
        SELECT id, name, description, created_at, last_used
        FROM workspaces
        ORDER BY last_used DESC;
    )";

    try {
        soci::rowset<soci::row> rs = (session->prepare << sql);
        for (const auto& row : rs) {
            Workspace workspace;
            workspace.id = row.get<int>(0);
            workspace.name = row.get<std::string>(1);

            workspace.description = convertRowValue(row, 2);
            if (workspace.description == "NULL")
                workspace.description = "";

            workspace.createdAt = convertRowValue(row, 3);
            if (workspace.createdAt == "NULL")
                workspace.createdAt = "";

            workspace.lastUsed = convertRowValue(row, 4);
            if (workspace.lastUsed == "NULL")
                workspace.lastUsed = "";

            workspaces.push_back(workspace);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to fetch workspaces: " << e.what() << std::endl;
    }

    return workspaces;
}

bool AppState::deleteWorkspace(const int workspaceId) const {
    if (workspaceId == 1)
        return false;

    // Transaction? SOCI supports transactions.
    soci::transaction tr(*session);
    try {
        // Move connections
        *session << "UPDATE saved_connections SET workspace_id = 1 WHERE workspace_id = :id",
            soci::use(workspaceId);

        // Delete workspace
        *session << "DELETE FROM workspaces WHERE id = :id", soci::use(workspaceId);

        tr.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to delete workspace: " << e.what() << std::endl;
        // Transaction rolled back automatically on destruction if not committed
        return false;
    }
}

bool AppState::updateWorkspaceLastUsed(const int workspaceId) const {
    const std::string sql = "UPDATE workspaces SET last_used = CURRENT_TIMESTAMP WHERE id = :id";
    try {
        *session << sql, soci::use(workspaceId);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to update workspace usage: " << e.what() << std::endl;
        return false;
    }
}

std::vector<SavedConnection> AppState::getConnectionsForWorkspace(const int workspaceId) const {
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id,
               show_all_databases
        FROM saved_connections
        WHERE workspace_id = :id
        ORDER BY last_used DESC;
    )";

    try {
        soci::rowset<soci::row> rs = (session->prepare << sql, soci::use(workspaceId));

        for (const auto& row : rs) {
            SavedConnection conn;
            conn.id = row.get<int>(0);
            conn.connectionInfo.name = row.get<std::string>(1);

            std::string typeStr = row.get<std::string>(2);
            if (typeStr == "sqlite") {
                conn.connectionInfo.type = DatabaseType::SQLITE;
            } else if (typeStr == "postgresql") {
                conn.connectionInfo.type = DatabaseType::POSTGRESQL;
            } else if (typeStr == "mysql") {
                conn.connectionInfo.type = DatabaseType::MYSQL;
            } else if (typeStr == "redis") {
                conn.connectionInfo.type = DatabaseType::REDIS;
            }

            conn.connectionInfo.host = row.get<std::string>(3, "");
            conn.connectionInfo.port = row.get<int>(4, 0);
            conn.connectionInfo.database = row.get<std::string>(5, "");

            std::string encryptedUsername = row.get<std::string>(6, "");
            std::string encryptedPassword = row.get<std::string>(7, "");
            std::string saltStr = row.get<std::string>(9, "");

            try {
                if (!saltStr.empty()) {
                    auto saltData = CryptoUtils::base64Decode(saltStr);
                    std::string salt(saltData.begin(), saltData.end());
                    std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);

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

            conn.connectionInfo.path = row.get<std::string>(8, "");

            conn.lastUsed = convertRowValue(row, 10);
            if (conn.lastUsed == "NULL")
                conn.lastUsed = "";

            conn.workspaceId = row.get<int>(11);
            conn.connectionInfo.showAllDatabases = row.get<int>(12) != 0;

            connections.push_back(conn);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to fetch workspace connections: " << e.what() << std::endl;
    }

    return connections;
}

bool AppState::moveConnectionToWorkspace(const int connectionId, const int workspaceId) const {
    const std::string sql = "UPDATE saved_connections SET workspace_id = :ws WHERE id = :id";
    try {
        *session << sql, soci::use(workspaceId), soci::use(connectionId);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to move connection: " << e.what() << std::endl;
        return false;
    }
}

bool AppState::ensureDefaultWorkspace() const {
    try {
        int count = 0;
        *session << "SELECT COUNT(*) FROM workspaces WHERE id = 1", soci::into(count);
        if (count > 0)
            return true;

        const std::string insertSql = R"(
            INSERT INTO workspaces (id, name, description, created_at, last_used)
            VALUES (1, 'Default', 'Default workspace for all connections', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);
        )";
        *session << insertSql;
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to ensure default workspace: " << e.what() << std::endl;
        return false;
    }
}
