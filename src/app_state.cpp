#include "app_state.hpp"
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>

AppState::AppState() : db(nullptr) {
    // Get user's home directory for storing app state
    const char *home = std::getenv("HOME");
    if (home) {
        dbPath = std::string(home) + "/.dear-sql/app_state.db";
    } else {
        dbPath = "./app_state.db";
    }
}

AppState::~AppState() {
    if (db) {
        sqlite3_close(db);
    }
}

bool AppState::initialize() {
    // Create directory if it doesn't exist
    std::filesystem::path dir = std::filesystem::path(dbPath).parent_path();
    if (!std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }

    int rc = sqlite3_open(dbPath.c_str(), &db);
    if (rc != SQLITE_OK) {
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
            path TEXT,
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

    return executeSQL(createConnectionsTable) && executeSQL(createSettingsTable);
}

bool AppState::executeSQL(const std::string &sql) {
    char *errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);

    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

bool AppState::saveConnection(const SavedConnection &connection) {
    const std::string sql = R"(
        INSERT OR REPLACE INTO saved_connections 
        (name, type, host, port, database_name, username, path, last_used)
        VALUES (?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, connection.name.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, connection.type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, connection.host.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, connection.port);
    sqlite3_bind_text(stmt, 5, connection.database.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, connection.username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, connection.path.c_str(), -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save connection: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    return true;
}

std::vector<SavedConnection> AppState::getSavedConnections() {
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, path, last_used 
        FROM saved_connections 
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);

    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
        return connections;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SavedConnection conn;
        conn.id = sqlite3_column_int(stmt, 0);
        conn.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        conn.type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));

        const char *host = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        conn.host = host ? host : "";

        conn.port = sqlite3_column_int(stmt, 4);

        const char *database = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        conn.database = database ? database : "";

        const char *username = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
        conn.username = username ? username : "";

        const char *path = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 7));
        conn.path = path ? path : "";

        const char *lastUsed = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 8));
        conn.lastUsed = lastUsed ? lastUsed : "";

        connections.push_back(conn);
    }

    sqlite3_finalize(stmt);
    return connections;
}

bool AppState::deleteConnection(int connectionId) {
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

bool AppState::updateLastUsed(int connectionId) {
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

bool AppState::setSetting(const std::string &key, const std::string &value) {
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

std::string AppState::getSetting(const std::string &key, const std::string &defaultValue) {
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
        const char *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
        if (value) {
            result = value;
        }
    }

    sqlite3_finalize(stmt);
    return result;
}
