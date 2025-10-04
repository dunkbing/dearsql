#include "ui/db_connection_dialog.hpp"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "utils/file_dialog.hpp"
#include "utils/spinner.hpp"
#include <imgui.h>
#include <iostream>
#include <memory>
#include <themes.hpp>

void DatabaseConnectionDialog::showDialog() {
    if (!isOpen) {
        isOpen = true;
        // Only set default state if not in edit mode
        if (!editingDatabase) {
            currentState = DialogState::TypeSelection;
            result = nullptr;
        }
        loadSavedConnections();
        ImGui::OpenPopup("Connect to Database");
    }

    // Render the dialog based on current state
    if (isOpen) {
        switch (currentState) {
        case DialogState::TypeSelection:
            renderTypeSelection();
            break;
        case DialogState::PostgreSQLConnection:
            renderSqlConnectionDialog(DatabaseType::POSTGRESQL);
            break;
        case DialogState::MySQLConnection:
            renderSqlConnectionDialog(DatabaseType::MYSQL);
            break;
        case DialogState::RedisConnection:
            renderRedisConnection();
            break;
        case DialogState::SavedConnections:
            renderSavedConnections();
            break;
        }
    }
}

void DatabaseConnectionDialog::renderTypeSelection() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Choose how to connect to a database:");
        ImGui::Separator();
        ImGui::Spacing();

        if (!savedConnections.empty()) {
            if (ImGui::Button("Saved Connections", ImVec2(-1, 30))) {
                currentState = DialogState::SavedConnections;
            }
            ImGui::Spacing();
            ImGui::Text("Or create a new connection:");
            ImGui::Spacing();
        }

        // radio buttons colors
        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface2);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.overlay0);
        ImGui::PushStyleColor(ImGuiCol_CheckMark, colors.blue);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);

        int selectedType = static_cast<int>(selectedDatabaseType);
        ImGui::RadioButton("SQLite File", &selectedType, static_cast<int>(DatabaseType::SQLITE));
        ImGui::Spacing();

        ImGui::RadioButton("PostgreSQL", &selectedType, static_cast<int>(DatabaseType::POSTGRESQL));
        ImGui::Spacing();

        ImGui::RadioButton("MySQL", &selectedType, static_cast<int>(DatabaseType::MYSQL));
        ImGui::Spacing();

        ImGui::RadioButton("Redis", &selectedType, static_cast<int>(DatabaseType::REDIS));
        ImGui::Spacing();

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar();

        selectedDatabaseType = static_cast<DatabaseType>(selectedType);

        ImGui::Separator();

        if (ImGui::Button("Next", ImVec2(100, 0))) {
            switch (selectedDatabaseType) {
            case DatabaseType::SQLITE: {
                // SQLite - directly open file dialog
                const auto db = createSQLiteDatabase();
                if (db) {
                    // Save SQLite connection to app state
                    SavedConnection conn;
                    conn.name = db->getName();
                    conn.type = "sqlite";
                    conn.path = db->getPath();
                    conn.workspaceId = Application::getInstance().getCurrentWorkspaceId();

                    const auto& app = Application::getInstance();
                    app.getAppState()->saveConnection(conn);

                    result = db;
                    ImGui::CloseCurrentPopup();
                    reset();
                }
                break;
            }
            case DatabaseType::POSTGRESQL:
                // Postgres - show connection dialog
                currentState = DialogState::PostgreSQLConnection;
                port = 5432; // Set default Postgres port
                break;
            case DatabaseType::MYSQL:
                // MySQL - show connection dialog
                currentState = DialogState::MySQLConnection;
                port = 3306; // Set default MySQL port
                break;
            case DatabaseType::REDIS:
                // Redis - show connection dialog
                currentState = DialogState::RedisConnection;
                port = 6379; // Set default Redis port
                break;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
            reset();
        }

        ImGui::EndPopup();
    }
}

void DatabaseConnectionDialog::renderSqlConnectionDialog(DatabaseType type) {
    const bool isPostgres = type == DatabaseType::POSTGRESQL;
    const char* typeLabel = isPostgres ? "Postgres" : "MySQL";

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(450, 500), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (editingDatabase) {
            ImGui::Text("Edit %s connection:", typeLabel);
        } else {
            ImGui::Text("Enter %s connection details:", typeLabel);
        }
        ImGui::Separator();
        ImGui::Spacing();

        const auto& colors = Application::getInstance().getCurrentColors();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.surface2);

        if (isConnecting) {
            ImGui::BeginDisabled();
        }

        ImGui::InputText("Connection Name", connectionName, sizeof(connectionName));
        ImGui::InputText("Host", host, sizeof(host));
        ImGui::InputInt("Port", &port);

        if (isPostgres) {
            ImGui::InputText("Database (?)", database, sizeof(database));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Leave empty to use the default 'postgres' database");
                ImGui::EndTooltip();
            }
        } else {
            ImGui::InputText("Database (optional)", database, sizeof(database));
        }

        ImGui::Spacing();
        ImGui::Text("Authentication:");
        ImGui::RadioButton("Username & Password", &authType, 0);
        ImGui::SameLine();
        ImGui::RadioButton("No Authentication", &authType, 1);
        ImGui::Spacing();

        if (authType == 0) {
            ImGui::InputText("Username", username, sizeof(username));
            ImGui::InputText("Password", password, sizeof(password), ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                if (editingDatabase) {
                    ImGui::Text("Leave empty to keep existing password");
                } else {
                    ImGui::Text("Password can be left empty if not required");
                }
                ImGui::EndTooltip();
            }
        } else {
            username[0] = '\0';
            password[0] = '\0';
        }

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Checkbox("Show all databases", &showAllDatabases);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("When checked, shows all databases from the server in the sidebar.\nWhen "
                        "unchecked, only shows the specified database.");
            ImGui::EndTooltip();
        }

        if (isConnecting) {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();

        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", errorMessage.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(errorMessage.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Copy error message to clipboard");
                ImGui::EndTooltip();
            }
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        ImGui::Separator();

        if (isConnecting) {
            checkAsyncConnectionStatus();
        }

        if (isConnecting) {
            ImGui::BeginDisabled();
            ImGui::Button("Connecting...", ImVec2(100, 0));
            ImGui::EndDisabled();

            ImGui::SameLine();
            ImGui::Text("%c", "|/-\\"[(int)(ImGui::GetTime() / 0.1f) & 3]);
        } else {
            const char* buttonLabel = editingDatabase ? "Update" : "Connect";
            if (ImGui::Button(buttonLabel, ImVec2(100, 0))) {
                startAsyncConnection();
            }
        }
        ImGui::SameLine();

        if (isConnecting) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Back", ImVec2(100, 0))) {
            currentState = DialogState::TypeSelection;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
            reset();
        }

        if (isConnecting) {
            ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }
}

void DatabaseConnectionDialog::renderRedisConnection() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (editingDatabase) {
            ImGui::Text("Edit Redis connection:");
        } else {
            ImGui::Text("Enter Redis connection details:");
        }
        ImGui::Separator();
        ImGui::Spacing();

        // Disable input fields during connection
        if (isConnecting) {
            ImGui::BeginDisabled();
        }

        ImGui::Text("Connection Name:");
        ImGui::InputText("##connection_name", connectionName, sizeof(connectionName));
        ImGui::Spacing();

        ImGui::Text("Host:");
        ImGui::InputText("##host", host, sizeof(host));
        ImGui::Spacing();

        ImGui::Text("Port:");
        ImGui::InputInt("##port", &port);
        ImGui::Spacing();

        ImGui::Text("Authentication:");
        ImGui::RadioButton("No Authentication", &authType, 1);
        ImGui::RadioButton("Username & Password", &authType, 0);
        ImGui::Spacing();

        if (authType == 0) {
            ImGui::Text("Username:");
            ImGui::InputText("##username", username, sizeof(username));
            ImGui::Spacing();

            ImGui::Text("Password:");
            ImGui::InputText("##password", password, sizeof(password),
                             ImGuiInputTextFlags_Password);
            ImGui::Spacing();
        }

        if (isConnecting) {
            ImGui::EndDisabled();
        }

        // Check for async connection completion
        if (isConnecting) {
            checkAsyncConnectionStatus();
        }

        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("Error: %s", errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        ImGui::Separator();

        if (isConnecting) {
            // Show disabled connect button with spinner
            ImGui::BeginDisabled();
            ImGui::Button("Connecting...", ImVec2(100, 0));
            ImGui::EndDisabled();

            // Improved spinner animation
            ImGui::SameLine();
            UIUtils::Spinner("##connecting_spinner", 8.0f, 3,
                             ImGui::GetColorU32(ImVec4(0.0f, 0.8f, 1.0f, 1.0f)));
        } else {
            const char* buttonLabel = editingDatabase ? "Update" : "Connect";
            if (ImGui::Button(buttonLabel, ImVec2(100, 0))) {
                startAsyncConnection();
            }
        }

        // Disable Back and Cancel buttons during connection
        if (isConnecting) {
            ImGui::BeginDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            currentState = DialogState::TypeSelection;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
            reset();
        }

        if (isConnecting) {
            ImGui::EndDisabled();
        }

        ImGui::EndPopup();
    }
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::getResult() {
    auto temp = result;
    result = nullptr; // Clear result after retrieval
    return temp;
}

void DatabaseConnectionDialog::reset() {
    isOpen = false;
    currentState = DialogState::TypeSelection;
    isConnecting = false;
    errorMessage.clear();
    selectedSavedConnection = -1;
    authType = 0;
    editingDatabase = nullptr;
    editingConnectionId = -1;
}

void DatabaseConnectionDialog::editConnection(std::shared_ptr<DatabaseInterface> db) {
    if (!db) {
        return;
    }

    // Clear previous state
    reset();

    // Fill in the connection details from the database
    strncpy(connectionName, db->getName().c_str(), sizeof(connectionName) - 1);
    connectionName[sizeof(connectionName) - 1] = '\0';

    editingConnectionId = db->getSavedConnectionId();

    if (db->getType() == DatabaseType::SQLITE) {
        selectedDatabaseType = DatabaseType::SQLITE;
        // SQLite doesn't need a connection dialog, just show type selection
        currentState = DialogState::TypeSelection;
    } else if (db->getType() == DatabaseType::POSTGRESQL) {
        selectedDatabaseType = DatabaseType::POSTGRESQL;
        currentState = DialogState::PostgreSQLConnection;

        // Get connection details from the database
        const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
        strncpy(host, pgDb->getHost().c_str(), sizeof(host) - 1);
        port = pgDb->getPort();
        strncpy(database, pgDb->getDatabase().c_str(), sizeof(database) - 1);
        strncpy(username, pgDb->getUsername().c_str(), sizeof(username) - 1);
        strncpy(password, pgDb->getPassword().c_str(), sizeof(password) - 1);
        showAllDatabases = pgDb->shouldShowAllDatabases();
        authType = pgDb->getUsername().empty() ? 1 : 0;
    } else if (db->getType() == DatabaseType::MYSQL) {
        selectedDatabaseType = DatabaseType::MYSQL;
        currentState = DialogState::MySQLConnection;

        // Get connection details from the database
        const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
        strncpy(host, mysqlDb->getHost().c_str(), sizeof(host) - 1);
        port = mysqlDb->getPort();
        strncpy(database, mysqlDb->getDatabase().c_str(), sizeof(database) - 1);
        strncpy(username, mysqlDb->getUsername().c_str(), sizeof(username) - 1);
        strncpy(password, mysqlDb->getPassword().c_str(), sizeof(password) - 1);
        showAllDatabases = mysqlDb->shouldShowAllDatabases();
        authType = mysqlDb->getUsername().empty() ? 1 : 0;
    } else if (db->getType() == DatabaseType::REDIS) {
        selectedDatabaseType = DatabaseType::REDIS;
        currentState = DialogState::RedisConnection;

        // Get connection details from the database
        const auto redisDb = std::dynamic_pointer_cast<RedisDatabase>(db);
        strncpy(host, redisDb->getHost().c_str(), sizeof(host) - 1);
        port = redisDb->getPort();
        strncpy(username, redisDb->getUsername().c_str(), sizeof(username) - 1);
        strncpy(password, redisDb->getPassword().c_str(), sizeof(password) - 1);
        authType = (redisDb->getPassword().empty() && redisDb->getUsername().empty()) ? 1 : 0;
    }

    editingDatabase = db;
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createSQLiteDatabase() {
    return FileDialog::openSQLiteFile();
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createSqlDatabase(
    const std::string& defaultDatabase, const std::optional<std::string>& passwordOverride,
    const std::function<std::shared_ptr<DatabaseInterface>(
        const std::string&, const std::string&, int, const std::string&, const std::string&,
        const std::string&, bool)>& factory) {
    if (strlen(connectionName) == 0) {
        return nullptr;
    }

    std::string usernameStr;
    std::string passwordStr;

    if (authType == 0) {
        if (strlen(username) == 0) {
            return nullptr;
        }
        usernameStr = std::string(username);
        passwordStr = passwordOverride.has_value() ? *passwordOverride : std::string(password);
    }

    std::string databaseStr = strlen(database) > 0 ? std::string(database) : defaultDatabase;

    return factory(std::string(connectionName), std::string(host), port, databaseStr, usernameStr,
                   passwordStr, showAllDatabases);
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createPostgreSQLDatabase(
    const std::optional<std::string>& passwordOverride) {
    return createSqlDatabase(
        "postgres", passwordOverride,
        [](const std::string& name, const std::string& hostValue, int portValue,
           const std::string& databaseValue, const std::string& usernameValue,
           const std::string& passwordValue, bool showAll) {
            return std::make_shared<PostgresDatabase>(name, hostValue, portValue, databaseValue,
                                                      usernameValue, passwordValue, showAll);
        });
}

std::shared_ptr<DatabaseInterface>
DatabaseConnectionDialog::createMySQLDatabase(const std::optional<std::string>& passwordOverride) {
    return createSqlDatabase(
        "mysql", passwordOverride,
        [](const std::string& name, const std::string& hostValue, int portValue,
           const std::string& databaseValue, const std::string& usernameValue,
           const std::string& passwordValue, bool showAll) {
            return std::make_shared<MySQLDatabase>(name, hostValue, portValue, databaseValue,
                                                   usernameValue, passwordValue, showAll);
        });
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createRedisDatabase() {
    if (strlen(connectionName) == 0) {
        std::cout << "Redis connection failed: Connection name is empty" << std::endl;
        return nullptr;
    }

    // For Redis with auth, username is optional (Redis 6+ ACL) but password is required
    std::string passwordStr = (authType == 0) ? std::string(password) : "";
    std::string usernameStr = (authType == 0) ? std::string(username) : "";

    std::cout << "Creating RedisDatabase: " << connectionName << " -> " << host << ":" << port
              << " (auth: " << (authType == 0 ? "username & password" : "none") << ")" << std::endl;
    if (authType == 0 && strlen(username) > 0) {
        std::cout << "Using username: " << username << std::endl;
    }

    return std::make_shared<RedisDatabase>(std::string(connectionName), std::string(host), port,
                                           passwordStr, usernameStr);
}

void DatabaseConnectionDialog::loadSavedConnections() {
    auto& app = Application::getInstance();
    savedConnections = app.getAppState()->getSavedConnections();
}

void DatabaseConnectionDialog::renderSavedConnections() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(500, 450), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Saved Database Connections:");
        ImGui::Separator();
        ImGui::Spacing();

        if (savedConnections.empty()) {
            ImGui::Text("No saved connections found.");
        } else {
            // List saved connections
            for (size_t i = 0; i < savedConnections.size(); i++) {
                const auto& conn = savedConnections[i];

                bool isSelected = (selectedSavedConnection == static_cast<int>(i));
                if (ImGui::Selectable((conn.name + " (" + conn.type + ")").c_str(), &isSelected)) {
                    selectedSavedConnection = static_cast<int>(i);
                }

                if (isSelected && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (conn.type == "postgresql") {
                        ImGui::Text("Host: %s:%d", conn.host.c_str(), conn.port);
                        ImGui::Text("Database: %s", conn.database.c_str());
                        ImGui::Text("Username: %s", conn.username.c_str());
                    } else if (conn.type == "mysql") {
                        ImGui::Text("Host: %s:%d", conn.host.c_str(), conn.port);
                        ImGui::Text("Database: %s", conn.database.c_str());
                        ImGui::Text("Username: %s", conn.username.c_str());
                    } else if (conn.type == "redis") {
                        ImGui::Text("Host: %s:%d", conn.host.c_str(), conn.port);
                        if (conn.password.empty()) {
                            ImGui::Text("Auth: None");
                        } else {
                            ImGui::Text("Auth: %s", conn.username.empty() ? "Password only"
                                                                          : "Username & Password");
                            if (!conn.username.empty()) {
                                ImGui::Text("Username: %s", conn.username.c_str());
                            }
                        }
                    } else {
                        ImGui::Text("Path: %s", conn.path.c_str());
                    }
                    ImGui::Text("Last used: %s", conn.lastUsed.c_str());
                    ImGui::EndTooltip();
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Button("Connect", ImVec2(100, 0)) && selectedSavedConnection >= 0) {
            const auto& conn = savedConnections[selectedSavedConnection];

            if (conn.type == "postgresql") {
                // Fill in the PostgreSQL fields and connect
                strncpy(connectionName, conn.name.c_str(), sizeof(connectionName) - 1);
                strncpy(host, conn.host.c_str(), sizeof(host) - 1);
                port = conn.port;
                strncpy(database, conn.database.c_str(), sizeof(database) - 1);
                strncpy(username, conn.username.c_str(), sizeof(username) - 1);
                strncpy(password, conn.password.c_str(), sizeof(password) - 1);
                showAllDatabases = conn.showAllDatabases;

                const auto db = createPostgreSQLDatabase();
                if (db) {
                    db->setSavedConnectionId(conn.id);
                    auto [success, error] = db->connect();
                    if (success) {
                        // Update last used timestamp
                        const auto& app = Application::getInstance();
                        app.getAppState()->updateLastUsed(conn.id);

                        result = db;
                        ImGui::CloseCurrentPopup();
                        reset();
                    } else {
                        errorMessage = "Failed to connect: " + error;
                    }
                }
            } else if (conn.type == "mysql") {
                // Fill in the MySQL fields and connect
                strncpy(connectionName, conn.name.c_str(), sizeof(connectionName) - 1);
                strncpy(host, conn.host.c_str(), sizeof(host) - 1);
                port = conn.port;
                strncpy(database, conn.database.c_str(), sizeof(database) - 1);
                strncpy(username, conn.username.c_str(), sizeof(username) - 1);
                strncpy(password, conn.password.c_str(), sizeof(password) - 1);
                showAllDatabases = conn.showAllDatabases;

                auto db = createMySQLDatabase();
                if (db) {
                    db->setSavedConnectionId(conn.id);
                    auto [success, error] = db->connect();
                    if (success) {
                        // Update last used timestamp
                        auto& app = Application::getInstance();
                        app.getAppState()->updateLastUsed(conn.id);

                        result = db;
                        ImGui::CloseCurrentPopup();
                        reset();
                    } else {
                        errorMessage = "Failed to connect: " + error;
                    }
                }
            } else if (conn.type == "redis") {
                // Fill in the Redis fields and connect
                strncpy(connectionName, conn.name.c_str(), sizeof(connectionName) - 1);
                strncpy(host, conn.host.c_str(), sizeof(host) - 1);
                port = conn.port;
                authType = conn.password.empty() ? 1 : 0;
                strncpy(username, conn.username.c_str(), sizeof(username) - 1);
                strncpy(password, conn.password.c_str(), sizeof(password) - 1);

                const auto db = createRedisDatabase();
                if (db) {
                    db->setSavedConnectionId(conn.id);
                    auto [success, error] = db->connect();
                    if (success) {
                        result = db;
                        ImGui::CloseCurrentPopup();
                        reset();
                    } else {
                        errorMessage = error;
                    }
                }
            } else if (conn.type == "sqlite") {
                // Create SQLite database from saved path
                const auto db = std::make_shared<SQLiteDatabase>(conn.name, conn.path);
                if (db) {
                    db->setSavedConnectionId(conn.id);
                    auto [success, error] = db->connect();
                    if (success) {
                        // Update last used timestamp
                        const auto& app = Application::getInstance();
                        app.getAppState()->updateLastUsed(conn.id);

                        result = db;
                        ImGui::CloseCurrentPopup();
                        reset();
                    } else {
                        errorMessage = "Failed to connect: " + error;
                    }
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Delete", ImVec2(100, 0)) && selectedSavedConnection >= 0) {
            const auto& app = Application::getInstance();
            app.getAppState()->deleteConnection(savedConnections[selectedSavedConnection].id);
            loadSavedConnections(); // Refresh list
            selectedSavedConnection = -1;
        }

        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            currentState = DialogState::TypeSelection;
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
            reset();
        }

        // Show error message if there is one
        if (!errorMessage.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", errorMessage.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(errorMessage.c_str());
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Copy error message to clipboard");
                ImGui::EndTooltip();
            }
            ImGui::PopStyleColor();
        }

        ImGui::EndPopup();
    }
}

void DatabaseConnectionDialog::startAsyncConnection() {
    // Clear previous error
    errorMessage.clear();
    isConnecting = true;

    // Start connection using std::async
    connectionFuture = std::async(std::launch::async, [this]() {
        try {
            // Try to create and connect to database based on current state
            std::shared_ptr<DatabaseInterface> db;

            std::optional<std::string> passwordOverride;
            if (authType == 0) {
                std::string providedPassword(password);
                if (providedPassword.empty() && editingConnectionId != -1) {
                    auto& app = Application::getInstance();
                    const auto savedConnections = app.getAppState()->getSavedConnections();
                    for (const auto& conn : savedConnections) {
                        if (conn.id == editingConnectionId) {
                            providedPassword = conn.password;
                            break;
                        }
                    }
                }

                if (editingConnectionId != -1 || !providedPassword.empty()) {
                    passwordOverride = providedPassword;
                }
            }

            switch (currentState) {
            case DialogState::PostgreSQLConnection:
                db = createPostgreSQLDatabase(passwordOverride);
                break;
            case DialogState::MySQLConnection:
                db = createMySQLDatabase(passwordOverride);
                break;
            case DialogState::RedisConnection:
                std::cout << "Creating Redis database connection..." << std::endl;
                db = createRedisDatabase();
                break;
            default:
                break;
            }

            if (db) {
                auto [success, error] = db->connect();
                if (success) {
                    return std::make_pair(db, std::string(""));
                } else {
                    return std::make_pair(std::shared_ptr<DatabaseInterface>(nullptr),
                                          std::string("Failed to connect: " + error));
                }
            } else {
                return std::make_pair(std::shared_ptr<DatabaseInterface>(nullptr),
                                      std::string("Please fill in all required fields"));
            }
        } catch (const std::exception& e) {
            return std::make_pair(std::shared_ptr<DatabaseInterface>(nullptr),
                                  std::string("Connection error: " + std::string(e.what())));
        }
    });
}

void DatabaseConnectionDialog::checkAsyncConnectionStatus() {
    if (connectionFuture.valid() &&
        connectionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto [db, error] = connectionFuture.get();

        if (db) {
            if (!editingDatabase) {
                // Save successful connection for new connections
                SavedConnection conn;
                conn.name = std::string(connectionName);
                switch (currentState) {
                case DialogState::PostgreSQLConnection:
                    conn.type = "postgresql";
                    break;
                case DialogState::MySQLConnection:
                    conn.type = "mysql";
                    break;
                case DialogState::RedisConnection:
                    conn.type = "redis";
                    break;
                default:
                    break;
                }
                conn.host = std::string(host);
                conn.port = port;
                conn.database = std::string(database);
                conn.username = std::string(username);
                conn.password = std::string(password);
                conn.showAllDatabases = showAllDatabases;
                conn.workspaceId = Application::getInstance().getCurrentWorkspaceId();

                const auto& app = Application::getInstance();
                if (app.getAppState()->saveConnection(conn)) {
                    const auto savedList = app.getAppState()->getSavedConnections();
                    for (const auto& saved : savedList) {
                        if (saved.workspaceId != conn.workspaceId || saved.type != conn.type ||
                            saved.name != conn.name) {
                            continue;
                        }
                        const bool matches =
                            (saved.type == "sqlite" && saved.path == conn.path) ||
                            (saved.type == "postgresql" && saved.host == conn.host &&
                             saved.port == conn.port && saved.database == conn.database) ||
                            (saved.type == "mysql" && saved.host == conn.host &&
                             saved.port == conn.port && saved.database == conn.database) ||
                            (saved.type == "redis" && saved.host == conn.host &&
                             saved.port == conn.port);

                        if (matches) {
                            db->setSavedConnectionId(saved.id);
                            break;
                        }
                    }
                }

                result = db;
            } else {
                // For edit mode, update the saved connection
                auto& app = Application::getInstance();

                if (editingConnectionId != -1) {
                    // Get the old connection to preserve password if needed
                    const auto savedConnections = app.getAppState()->getSavedConnections();
                    std::string oldPassword;
                    for (const auto& conn : savedConnections) {
                        if (conn.id == editingConnectionId) {
                            oldPassword = conn.password;
                            break;
                        }
                    }

                    // Update the connection
                    SavedConnection updatedConn;
                    updatedConn.id = editingConnectionId;
                    updatedConn.name = std::string(connectionName);
                    switch (currentState) {
                    case DialogState::PostgreSQLConnection:
                        updatedConn.type = "postgresql";
                        break;
                    case DialogState::MySQLConnection:
                        updatedConn.type = "mysql";
                        break;
                    case DialogState::RedisConnection:
                        updatedConn.type = "redis";
                        break;
                    default:
                        break;
                    }
                    updatedConn.host = std::string(host);
                    updatedConn.port = port;
                    updatedConn.database = std::string(database);
                    updatedConn.username = std::string(username);
                    // If password is empty, keep the old one
                    updatedConn.password =
                        strlen(password) > 0 ? std::string(password) : oldPassword;
                    updatedConn.showAllDatabases = showAllDatabases;
                    updatedConn.workspaceId = app.getCurrentWorkspaceId();
                    app.getAppState()->updateConnection(updatedConn);
                }

                if (editingConnectionId != -1) {
                    db->setSavedConnectionId(editingConnectionId);
                }

                // Update the database in the application
                auto& databases = app.getDatabases();
                for (size_t i = 0; i < databases.size(); i++) {
                    if (databases[i] == editingDatabase) {
                        // Disconnect old database
                        databases[i]->disconnect();
                        // Replace with new database
                        databases[i] = db;
                        break;
                    }
                }

                result = nullptr; // Don't return a new database in edit mode
            }
            ImGui::CloseCurrentPopup();
            reset();
        } else {
            isConnecting = false;
            errorMessage = error;
        }
    }
}
