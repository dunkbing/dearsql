#pragma once

#include <memory>
#include <sqlite3.h>
#include <string>
#include <vector>

struct SavedConnection {
    int id;
    std::string name;
    std::string type; // "sqlite" or "postgresql"
    std::string host;
    int port;
    std::string database;
    std::string username;
    std::string path; // for SQLite files
    std::string lastUsed;
};

class AppState {
public:
    AppState();
    ~AppState();

    // Initialize the app state database
    bool initialize();

    // Connection history management
    bool saveConnection(const SavedConnection &connection);
    std::vector<SavedConnection> getSavedConnections();
    bool deleteConnection(int connectionId);
    bool updateLastUsed(int connectionId);

    // Settings management
    bool setSetting(const std::string &key, const std::string &value);
    std::string getSetting(const std::string &key, const std::string &defaultValue = "");

private:
    sqlite3 *db;
    std::string dbPath;

    bool createTables();
    bool executeSQL(const std::string &sql);
};
