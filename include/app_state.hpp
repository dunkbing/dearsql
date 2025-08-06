#pragma once

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
    int workspaceId = 1; // Associated workspace ID, defaults to 1 (default workspace)
};

struct Workspace {
    int id;
    std::string name;
    std::string description;
    std::string createdAt;
    std::string lastUsed;
};

class AppState {
public:
    AppState();
    ~AppState();

    // Initialize the app state database
    bool initialize();

    // Connection history management
    [[nodiscard]] bool saveConnection(const SavedConnection &connection) const;
    [[nodiscard]] std::vector<SavedConnection> getSavedConnections() const;
    [[nodiscard]] bool deleteConnection(int connectionId) const;
    [[nodiscard]] bool updateLastUsed(int connectionId) const;

    // Settings management
    [[nodiscard]] bool setSetting(const std::string &key, const std::string &value) const;
    [[nodiscard]] std::string getSetting(const std::string &key,
                                         const std::string &defaultValue = "") const;

    // Workspace management
    [[nodiscard]] int saveWorkspace(const Workspace &workspace) const;
    [[nodiscard]] std::vector<Workspace> getWorkspaces() const;
    [[nodiscard]] bool deleteWorkspace(int workspaceId) const;
    [[nodiscard]] bool updateWorkspaceLastUsed(int workspaceId) const;
    [[nodiscard]] std::vector<SavedConnection> getConnectionsForWorkspace(int workspaceId) const;
    [[nodiscard]] bool moveConnectionToWorkspace(int connectionId, int workspaceId) const;
    [[nodiscard]] bool ensureDefaultWorkspace() const;

private:
    sqlite3 *db;
    std::string dbPath;

    bool createTables();
    [[nodiscard]] bool executeSQL(const std::string &sql) const;
};
