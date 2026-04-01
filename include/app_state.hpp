#pragma once

#include "database/db_interface.hpp"
#include <sqlite3.h>
#include <string>
#include <vector>

struct SqlScript {
    int id = 0;
    std::string name;         // display name / filename without extension
    std::string filePath;     // full path on disk
    int connectionId = 0;     // linked connection (0 = unlinked)
    std::string databaseName; // database within the connection
    std::string schemaName;   // schema within the database (postgres)
    std::string createdAt;
    std::string updatedAt;
};

struct SavedConnection {
    int id = 0;
    DatabaseConnectionInfo connectionInfo;
    std::string lastUsed;
    int workspaceId = 1; // default workspace
};

struct Workspace {
    int id = 0;
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
    int saveConnection(const SavedConnection& connection) const;
    bool updateConnection(const SavedConnection& connection) const;
    [[nodiscard]] std::vector<SavedConnection> getSavedConnections() const;
    [[nodiscard]] int getConnectionCount() const;
    bool deleteConnection(int connectionId) const;
    bool renameConnection(int connectionId, const std::string& newName) const;
    bool updateLastUsed(int connectionId) const;

    // Settings management
    bool setSetting(const std::string& key, const std::string& value) const;
    [[nodiscard]] std::string getSetting(const std::string& key,
                                         const std::string& defaultValue = "") const;

    // Workspace management
    [[nodiscard]] int saveWorkspace(const Workspace& workspace) const;
    [[nodiscard]] std::vector<Workspace> getWorkspaces() const;
    [[nodiscard]] bool deleteWorkspace(int workspaceId) const;
    [[nodiscard]] bool renameWorkspace(int workspaceId, const std::string& name) const;
    bool updateWorkspaceLastUsed(int workspaceId) const;
    [[nodiscard]] std::vector<SavedConnection> getConnectionsForWorkspace(int workspaceId) const;
    [[nodiscard]] bool moveConnectionToWorkspace(int connectionId, int workspaceId) const;
    bool ensureDefaultWorkspace() const;

    // SQL script management
    int saveScript(const SqlScript& script) const;
    bool updateScript(const SqlScript& script) const;
    bool deleteScript(int scriptId) const;
    [[nodiscard]] std::vector<SqlScript> getScriptsForConnection(int connectionId) const;

private:
    sqlite3* db_ = nullptr;
    std::string dbPath;

    bool createTables();
    bool executeSQL(const std::string& sql) const;
};
