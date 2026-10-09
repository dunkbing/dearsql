#pragma once

#include "database/db_interface.hpp"
#include <sqlite3.h>
#include <string>
#include <vector>

// one assistant chat, under a connection. transcript is stored only for the api-key backend;
// acp agents keep their own history and are resumed through acp_session_id
struct AiSession {
    int id = 0;
    std::string backend; // agent id, "custom" or "api"
    std::string acpSessionId;
    std::string title; // first user message
    std::string updatedAt;
};

// "5m", "2h", "3d" since an ai_sessions updated_at (sqlite utc CURRENT_TIMESTAMP)
std::string relativeAge(const std::string& stamp);

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

    // Every method on this class talks to SQLite. None of them are field
    // accessors, and the UI rebuilds every frame, so calling one from a render
    // path turns it into a per-frame query. Read once and cache.
    //
    // getSavedConnections is the sharpest edge: it derives a key and decrypts
    // credentials per row, costing tens of milliseconds for a handful of
    // connections.

    // Connection history management
    int saveConnection(const SavedConnection& connection) const;
    bool updateConnection(const SavedConnection& connection) const;
    [[nodiscard]] std::vector<SavedConnection> getSavedConnections() const;
    [[nodiscard]] int getConnectionCount() const;
    bool deleteConnection(int connectionId) const;
    bool renameConnection(int connectionId, const std::string& newName) const;
    bool updateConnectionEnvTag(int connectionId, const std::string& envTag) const;
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

    // AI sessions. id 0 inserts; returns the row id (or -1)
    int saveAiSession(int id, int connectionId, const std::string& backend,
                      const std::string& acpSessionId, const std::string& title,
                      const std::string& transcript) const;
    // a connection's chats, newest first; empty backend = every backend
    [[nodiscard]] std::vector<AiSession>
    getAiSessions(int connectionId, const std::string& backend = "", int limit = 20) const;
    [[nodiscard]] std::string getAiSessionTranscript(int id) const;
    bool deleteAiSession(int id) const;

private:
    sqlite3* db_ = nullptr;
    std::string dbPath;

    bool createTables();
    bool executeSQL(const std::string& sql) const;
    void migrateCredentialKeys() const;
    bool updateSavedConnectionColumn(const char* column, const std::string& value,
                                     int connectionId) const;
};
