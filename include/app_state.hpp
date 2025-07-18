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
    std::string password;
    std::string path; // sqlite files
    std::string lastUsed;
};

class AppState {
public:
    AppState();
    ~AppState();

    // Initialize the app state database
    bool initialize();

    // Connection history management
    bool saveConnection(const SavedConnection &connection) const;
    std::vector<SavedConnection> getSavedConnections() const;
    bool deleteConnection(int connectionId) const;
    bool updateLastUsed(int connectionId) const;

    // Settings management
    bool setSetting(const std::string &key, const std::string &value) const;
    std::string getSetting(const std::string &key, const std::string &defaultValue = "") const;

private:
    sqlite3 *db;
    std::string dbPath;

    bool createTables();
    bool executeSQL(const std::string &sql) const;
};
