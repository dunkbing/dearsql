#include "app_state.hpp"
#include "utils/crypto.hpp"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

AppState::AppState() : db(nullptr) {
    fs::path dbPath_;

    if (const char *home = std::getenv("HOME")) {
        dbPath_ = fs::path(home) / ".dear-sql" / "app_state.db";
    } else {
        dbPath_ = fs::path("./app_state.db");
    }

    dbPath = dbPath_.string();
}

AppState::~AppState() {
    if (db) {
        sqlite3_close(db);
    }
}

bool AppState::initialize() {
    // Create directory if it doesn't exist
    if (const fs::path dir = fs::path(dbPath).parent_path(); !fs::exists(dir)) {
        fs::create_directories(dir);
    }

    if (const int rc = sqlite3_open(dbPath.c_str(), &db); rc != SQLITE_OK) {
        std::cerr << "Failed to open app state database: " << sqlite3_errmsg(db) << std::endl;
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
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
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

    // Add workspace_id column to existing saved_connections table if it doesn't exist
    const std::string addWorkspaceIdColumn = R"(
        ALTER TABLE saved_connections ADD COLUMN workspace_id INTEGER DEFAULT 1;
    )";

    bool success = executeSQL(createConnectionsTable) && executeSQL(createSettingsTable) && executeSQL(createWorkspacesTable);
    
    // Try to add workspace_id column (will fail silently if it already exists)
    executeSQL(addWorkspaceIdColumn);
    
    // Ensure default workspace exists
    if (success) {
        ensureDefaultWorkspace();
    }
    
    return success;
}

bool AppState::executeSQL(const std::string &sql) const {
    char *errMsg = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool AppState::saveConnection(const SavedConnection &connection) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO saved_connections
        (name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, ?);
    )";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);
    std::string encryptedUsername =
        connection.username.empty() ? "" : CryptoUtils::encrypt(connection.username, encryptionKey);
    std::string encryptedPassword =
        connection.password.empty() ? "" : CryptoUtils::encrypt(connection.password, encryptionKey);

    sqlite3_bind_text(stmt, 1, connection.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, connection.type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, connection.host.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, connection.port);
    sqlite3_bind_text(stmt, 5, connection.database.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, encryptedUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, encryptedPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, connection.path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(
        stmt, 9, CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end())).c_str(),
        -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 10, connection.workspaceId);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save connection: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    return true;
}

std::vector<SavedConnection> AppState::getSavedConnections() const {
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used, 
               COALESCE(workspace_id, 1) as workspace_id
        FROM saved_connections
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt *stmt;
    const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return connections;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavedConnection conn;
        conn.id = sqlite3_column_int(stmt, 0);
        conn.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        conn.type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));

        const auto host = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        conn.host = host ? host : "";

        conn.port = sqlite3_column_int(stmt, 4);

        const auto database = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        conn.database = database ? database : "";

        // Decrypt sensitive data
        const auto *encryptedUsername =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
        const auto *encryptedPassword =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        const auto *saltStr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9));

        try {
            if (saltStr && strlen(saltStr) > 0) {
                auto saltData = CryptoUtils::base64Decode(saltStr);
                std::string salt(saltData.begin(), saltData.end());
                std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);

                // Decrypt username
                if (encryptedUsername && strlen(encryptedUsername) > 0) {
                    try {
                        conn.username = CryptoUtils::decrypt(encryptedUsername, encryptionKey);
                    } catch (const std::exception &e) {
                        std::cerr << "Failed to decrypt username for connection " << conn.name
                                  << ": " << e.what() << std::endl;
                        conn.username = "";
                    }
                } else {
                    conn.username = "";
                }

                // Decrypt password
                if (encryptedPassword && strlen(encryptedPassword) > 0) {
                    try {
                        conn.password = CryptoUtils::decrypt(encryptedPassword, encryptionKey);
                    } catch (const std::exception &e) {
                        std::cerr << "Failed to decrypt password for connection " << conn.name
                                  << ": " << e.what() << std::endl;
                        conn.password = "";
                    }
                } else {
                    conn.password = "";
                }
            } else {
                conn.username = encryptedUsername ? encryptedUsername : "";
                conn.password = encryptedPassword ? encryptedPassword : "";
            }
        } catch (const std::exception &e) {
            std::cerr << "Failed to process credentials for connection " << conn.name << ": "
                      << e.what() << std::endl;
            conn.username = "";
            conn.password = "";
        }

        const auto *path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8));
        conn.path = path ? path : "";

        const auto *lastUsed = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 10));
        conn.lastUsed = lastUsed ? lastUsed : "";
        
        conn.workspaceId = sqlite3_column_int(stmt, 11);

        connections.push_back(conn);
    }

    sqlite3_finalize(stmt);
    return connections;
}

bool AppState::deleteConnection(const int connectionId) const {
    const std::string sql = "DELETE FROM saved_connections WHERE id = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, connectionId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool AppState::updateLastUsed(const int connectionId) const {
    const std::string sql =
        "UPDATE saved_connections SET last_used = CURRENT_TIMESTAMP WHERE id = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, connectionId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool AppState::setSetting(const std::string &key, const std::string &value) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO app_settings (key, value, updated_at)
        VALUES (?, ?, CURRENT_TIMESTAMP);
    )";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::string AppState::getSetting(const std::string &key, const std::string &defaultValue) const {
    const std::string sql = "SELECT value FROM app_settings WHERE key = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return defaultValue;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);

    std::string result = defaultValue;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        if (value) {
            result = value;
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

bool AppState::saveWorkspace(const Workspace &workspace) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO workspaces (name, description, last_used)
        VALUES (?, ?, CURRENT_TIMESTAMP);
    )";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare workspace statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, workspace.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, workspace.description.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<Workspace> AppState::getWorkspaces() const {
    std::vector<Workspace> workspaces;

    const std::string sql = R"(
        SELECT id, name, description, created_at, last_used
        FROM workspaces
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt *stmt;
    const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare workspace statement: " << sqlite3_errmsg(db) << std::endl;
        return workspaces;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Workspace workspace;
        workspace.id = sqlite3_column_int(stmt, 0);
        workspace.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        
        const auto *description = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        workspace.description = description ? description : "";
        
        const auto *createdAt = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        workspace.createdAt = createdAt ? createdAt : "";
        
        const auto *lastUsed = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        workspace.lastUsed = lastUsed ? lastUsed : "";
        
        workspaces.push_back(workspace);
    }

    sqlite3_finalize(stmt);
    return workspaces;
}

bool AppState::deleteWorkspace(const int workspaceId) const {
    // Don't allow deletion of default workspace
    if (workspaceId == 1) {
        return false;
    }

    // Move all connections to default workspace before deleting
    const std::string moveConnectionsSql = "UPDATE saved_connections SET workspace_id = 1 WHERE workspace_id = ?;";
    sqlite3_stmt *moveStmt;
    int rc = sqlite3_prepare_v2(db, moveConnectionsSql.c_str(), -1, &moveStmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(moveStmt, 1, workspaceId);
        sqlite3_step(moveStmt);
        sqlite3_finalize(moveStmt);
    }

    // Delete the workspace
    const std::string sql = "DELETE FROM workspaces WHERE id = ?;";
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare workspace deletion statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, workspaceId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool AppState::updateWorkspaceLastUsed(const int workspaceId) const {
    const std::string sql = "UPDATE workspaces SET last_used = CURRENT_TIMESTAMP WHERE id = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare workspace update statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, workspaceId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

std::vector<SavedConnection> AppState::getConnectionsForWorkspace(const int workspaceId) const {
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id
        FROM saved_connections
        WHERE workspace_id = ?
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt *stmt;
    const int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare workspace connections statement: " << sqlite3_errmsg(db) << std::endl;
        return connections;
    }

    sqlite3_bind_int(stmt, 1, workspaceId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavedConnection conn;
        conn.id = sqlite3_column_int(stmt, 0);
        conn.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        conn.type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));

        const auto host = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        conn.host = host ? host : "";

        conn.port = sqlite3_column_int(stmt, 4);

        const auto database = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        conn.database = database ? database : "";

        // Decrypt sensitive data (similar to getSavedConnections)
        const auto *encryptedUsername = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
        const auto *encryptedPassword = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        const auto *saltStr = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9));

        try {
            if (saltStr && strlen(saltStr) > 0) {
                auto saltData = CryptoUtils::base64Decode(saltStr);
                std::string salt(saltData.begin(), saltData.end());
                std::string encryptionKey = CryptoUtils::deriveKey("dear-sql-master-key", salt);

                if (encryptedUsername && strlen(encryptedUsername) > 0) {
                    try {
                        conn.username = CryptoUtils::decrypt(encryptedUsername, encryptionKey);
                    } catch (const std::exception &e) {
                        conn.username = "";
                    }
                } else {
                    conn.username = "";
                }

                if (encryptedPassword && strlen(encryptedPassword) > 0) {
                    try {
                        conn.password = CryptoUtils::decrypt(encryptedPassword, encryptionKey);
                    } catch (const std::exception &e) {
                        conn.password = "";
                    }
                } else {
                    conn.password = "";
                }
            } else {
                conn.username = encryptedUsername ? encryptedUsername : "";
                conn.password = encryptedPassword ? encryptedPassword : "";
            }
        } catch (const std::exception &e) {
            conn.username = "";
            conn.password = "";
        }

        const auto *path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8));
        conn.path = path ? path : "";

        const auto *lastUsed = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 10));
        conn.lastUsed = lastUsed ? lastUsed : "";
        
        conn.workspaceId = sqlite3_column_int(stmt, 11);

        connections.push_back(conn);
    }

    sqlite3_finalize(stmt);
    return connections;
}

bool AppState::moveConnectionToWorkspace(const int connectionId, const int workspaceId) const {
    const std::string sql = "UPDATE saved_connections SET workspace_id = ? WHERE id = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare move connection statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, workspaceId);
    sqlite3_bind_int(stmt, 2, connectionId);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool AppState::ensureDefaultWorkspace() const {
    // Check if default workspace exists
    const std::string checkSql = "SELECT COUNT(*) FROM workspaces WHERE id = 1;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, checkSql.c_str(), -1, &stmt, nullptr);
    
    if (rc != SQLITE_OK) {
        return false;
    }
    
    bool exists = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        exists = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    
    if (exists) {
        return true;
    }
    
    // Create default workspace
    const std::string insertSql = R"(
        INSERT INTO workspaces (id, name, description, created_at, last_used)
        VALUES (1, 'Default', 'Default workspace for all connections', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);
    )";
    
    return executeSQL(insertSql);
}
