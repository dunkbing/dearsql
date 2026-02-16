#include "ui/db_connection_dialog.hpp"
#include "application.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"
#include "utils/file_dialog.hpp"
#include "utils/spinner.hpp"
#include <cstring>
#include <imgui.h>
#include <iostream>
#include <memory>
#include <themes.hpp>

namespace {
    constexpr const char* DIALOG_TITLE = "Connect to Database";
    constexpr const char* DEFAULT_CONNECTION_NAME = "Untitled connection";

    bool isDefaultName(const char* name) {
        return strcmp(name, DEFAULT_CONNECTION_NAME) == 0;
    }
} // namespace

void DatabaseConnectionDialog::showDialog() {
    if (!isOpen) {
        Logger::debug(std::format("showDialog: Opening dialog, editingDatabase={}, currentState={}",
                                  editingDatabase ? "set" : "null",
                                  static_cast<int>(currentState)));
        isOpen = true;
        if (!editingDatabase) {
            Logger::debug("showDialog: No editingDatabase, resetting to NewConnection");
            currentState = DialogState::NewConnection;
            result = nullptr;
            strncpy(connectionName, DEFAULT_CONNECTION_NAME, sizeof(connectionName) - 1);
        } else {
            Logger::debug(std::format("showDialog: editingDatabase set, keeping currentState={}",
                                      static_cast<int>(currentState)));
        }
        ImGui::OpenPopup(DIALOG_TITLE);
    }

    renderConnectionDialog();
}

void DatabaseConnectionDialog::renderDatabaseTypeSelector() {
    const char* typeNames[] = {"SQLite", "PostgreSQL", "MySQL", "Redis", "MongoDB"};
    int currentType = static_cast<int>(selectedDatabaseType);

    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("Type", typeNames[currentType])) {
        for (int i = 0; i < 5; i++) {
            bool isSelected = (currentType == i);
            if (ImGui::Selectable(typeNames[i], isSelected)) {
                DatabaseType newType = static_cast<DatabaseType>(i);
                if (newType != selectedDatabaseType) {
                    selectedDatabaseType = newType;
                    // Update default port and connection name
                    bool shouldSetDefault =
                        strlen(connectionName) == 0 || isDefaultName(connectionName);
                    switch (selectedDatabaseType) {
                    case DatabaseType::SQLITE:
                        break;
                    case DatabaseType::POSTGRESQL:
                        port = 5432;
                        break;
                    case DatabaseType::MYSQL:
                        port = 3306;
                        break;
                    case DatabaseType::MONGODB:
                        port = 27017;
                        break;
                    case DatabaseType::REDIS:
                        port = 6379;
                        break;
                    }
                    if (shouldSetDefault)
                        strncpy(connectionName, DEFAULT_CONNECTION_NAME,
                                sizeof(connectionName) - 1);
                }
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void DatabaseConnectionDialog::renderSQLiteFields() {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::Text("Database File Path:");
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface0);

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
    ImGui::InputText("##sqlite_path", sqlitePath, sizeof(sqlitePath));

    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(80, 0))) {
        auto db = FileDialog::openSQLiteFile();
        if (db) {
            auto sqliteDb = std::dynamic_pointer_cast<SQLiteDatabase>(db);
            if (sqliteDb) {
                strncpy(sqlitePath, sqliteDb->getPath().c_str(), sizeof(sqlitePath) - 1);
                sqlitePath[sizeof(sqlitePath) - 1] = '\0';

                if (strlen(connectionName) == 0 || isDefaultName(connectionName)) {
                    strncpy(connectionName, sqliteDb->getConnectionInfo().name.c_str(),
                            sizeof(connectionName) - 1);
                    connectionName[sizeof(connectionName) - 1] = '\0';
                }
            }
        }
    }
    ImGui::Spacing();
}

void DatabaseConnectionDialog::renderServerFields(bool showDatabase, const char* databaseTooltip) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.surface2);

    // Host and Port on same line
    ImGui::SetNextItemWidth(300);
    ImGui::InputText("Host", host, sizeof(host));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    if (ImGui::InputText("Port", portStr, sizeof(portStr), ImGuiInputTextFlags_CharsDecimal)) {
        port = atoi(portStr);
        if (port <= 0)
            port = 1;
        if (port > 65535)
            port = 65535;
    }

    if (showDatabase) {
        if (databaseTooltip) {
            ImGui::InputText("Database (?)", database, sizeof(database));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("%s", databaseTooltip);
                ImGui::EndTooltip();
            }
        } else {
            ImGui::InputText("Database (optional)", database, sizeof(database));
        }
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::Spacing();
}

void DatabaseConnectionDialog::renderAuthFields(bool defaultNoAuth) {
    const auto& colors = Application::getInstance().getCurrentColors();

    ImGui::Text("Authentication:");
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);

    if (defaultNoAuth) {
        ImGui::RadioButton("No Authentication", &authType, AUTH_NONE);
        ImGui::SameLine();
        ImGui::RadioButton("Username & Password", &authType, AUTH_USERNAME_PASSWORD);
    } else {
        ImGui::RadioButton("Username & Password", &authType, AUTH_USERNAME_PASSWORD);
        ImGui::SameLine();
        ImGui::RadioButton("No Authentication", &authType, AUTH_NONE);
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::Spacing();

    if (authType == AUTH_USERNAME_PASSWORD) {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.mantle);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface0);

        // Username and Password on same line
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Username", username, sizeof(username));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Password", password, sizeof(password), ImGuiInputTextFlags_Password);

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Password can be left empty if not required");
            ImGui::EndTooltip();
        }
    } else {
        username[0] = '\0';
        password[0] = '\0';
    }
    ImGui::Spacing();
}

void DatabaseConnectionDialog::renderShowAllDatabasesCheckbox() {
    ImGui::Checkbox("Show all databases", &showAllDatabases);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("When checked, shows all databases from the server in the sidebar.\nWhen "
                    "unchecked, only shows the specified database.");
        ImGui::EndTooltip();
    }
    ImGui::Spacing();
}

void DatabaseConnectionDialog::renderConnectionDialog() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(580, 0), ImVec2(580, FLT_MAX));

    const auto& colors = Application::getInstance().getCurrentColors();

    // Square corners for window and child elements
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.0f);

    if (ImGui::BeginPopupModal(DIALOG_TITLE, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {

        if (isConnecting) {
            ImGui::BeginDisabled();
        }

        // Connection name and database type on same line
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.mantle);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface0);

        ImGui::SetNextItemWidth(300);
        ImGui::InputText("Name", connectionName, sizeof(connectionName));

        ImGui::SameLine();
        if (editingDatabase) {
            ImGui::BeginDisabled();
        }
        renderDatabaseTypeSelector();
        if (editingDatabase) {
            ImGui::EndDisabled();
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        ImGui::Separator();
        ImGui::Spacing();

        // Type-specific fields
        switch (selectedDatabaseType) {
        case DatabaseType::SQLITE:
            renderSQLiteFields();
            break;

        case DatabaseType::POSTGRESQL: {
            renderServerFields(true, "Leave empty to use the default 'postgres' database");

            // SSL Mode selector
            const auto& sslColors = Application::getInstance().getCurrentColors();
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, sslColors.overlay1);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, sslColors.mantle);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, sslColors.surface0);
            static const char* sslModes[] = {"disable", "allow",     "prefer",
                                             "require", "verify-ca", "verify-full"};
            ImGui::SetNextItemWidth(150);
            ImGui::Combo("SSL Mode", &sslModeIndex, sslModes, IM_ARRAYSIZE(sslModes));
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();
            ImGui::Spacing();

            renderAuthFields(false);
            renderShowAllDatabasesCheckbox();
            break;
        }

        case DatabaseType::MYSQL:
            renderServerFields(true, nullptr);
            renderAuthFields(false);
            renderShowAllDatabasesCheckbox();
            break;

        case DatabaseType::MONGODB:
            renderServerFields(true, nullptr);
            renderAuthFields(true);
            renderShowAllDatabasesCheckbox();
            break;

        case DatabaseType::REDIS:
            renderServerFields(false, nullptr);
            renderAuthFields(true);
            break;
        }

        if (isConnecting) {
            ImGui::EndDisabled();
        }

        // Error message
        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", errorMessage.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy")) {
                ImGui::SetClipboardText(errorMessage.c_str());
            }
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        ImGui::Separator();

        // Check async connection status
        if (isConnecting) {
            checkAsyncConnectionStatus();
        }

        // Buttons
        if (connectionOp.isRunning()) {
            UIUtils::Spinner("##connecting", 8.0f, 3, ImGui::GetColorU32(colors.blue));
            ImGui::SameLine();
            ImGui::Text("Connecting...");
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 0))) {
                connectionOp.cancel();
                isConnecting = false;
                errorMessage = "Connection cancelled";
            }
        } else {
            const char* buttonLabel = editingDatabase ? "Update" : "Connect";
            if (ImGui::Button(buttonLabel, ImVec2(100, 0))) {
                if (selectedDatabaseType == DatabaseType::SQLITE) {
                    // Handle SQLite synchronously
                    if (strlen(sqlitePath) > 0) {
                        DatabaseConnectionInfo connInfo;
                        connInfo.type = DatabaseType::SQLITE;
                        connInfo.name = std::string(connectionName);
                        connInfo.path = std::string(sqlitePath);

                        auto db = std::make_shared<SQLiteDatabase>(connInfo);
                        if (db) {
                            auto [success, error] = db->connect();
                            if (success) {
                                SavedConnection conn;
                                conn.connectionInfo = connInfo;
                                conn.workspaceId =
                                    Application::getInstance().getCurrentWorkspaceId();

                                const auto& app = Application::getInstance();
                                int newConnectionId = app.getAppState()->saveConnection(conn);
                                if (newConnectionId != -1) {
                                    db->setConnectionId(newConnectionId);
                                }

                                result = db;
                                ImGui::CloseCurrentPopup();
                                reset();
                            } else {
                                errorMessage = "Failed to connect: " + error;
                            }
                        }
                    } else {
                        errorMessage = "Please select a database file";
                    }
                } else {
                    startAsyncConnection();
                }
            }
        }

        if (!isConnecting) {
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                ImGui::CloseCurrentPopup();
                reset();
            }
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3); // WindowRounding, FrameRounding, PopupRounding
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::getResult() {
    auto temp = result;
    result = nullptr; // Clear result after retrieval
    return temp;
}

void DatabaseConnectionDialog::reset() {
    isOpen = false;
    currentState = DialogState::NewConnection;
    isConnecting = false;
    errorMessage.clear();
    authType = AUTH_USERNAME_PASSWORD;
    sslModeIndex = 2; // "prefer"
    editingDatabase = nullptr;
    editingConnectionId = -1;
    sqlitePath[0] = '\0';
}

void DatabaseConnectionDialog::editConnection(const std::shared_ptr<DatabaseInterface>& db) {
    if (!db) {
        Logger::warn("editConnection called with null db");
        return;
    }

    Logger::info(std::format(
        "editConnection: name='{}', type={}, connectionId={}", db->getConnectionInfo().name,
        static_cast<int>(db->getConnectionInfo().type), db->getConnectionId()));

    // Clear previous state
    reset();

    // Fill in the connection details from the database
    strncpy(connectionName, db->getConnectionInfo().name.c_str(), sizeof(connectionName) - 1);
    connectionName[sizeof(connectionName) - 1] = '\0';

    editingConnectionId = db->getConnectionId();
    auto const type = db->getConnectionInfo().type;
    selectedDatabaseType = type;
    currentState = DialogState::NewConnection;

    Logger::debug(std::format("editConnection: After reset, type={}", static_cast<int>(type)));

    if (type == DatabaseType::SQLITE) {
        strncpy(sqlitePath, db->getConnectionInfo().path.c_str(), sizeof(sqlitePath) - 1);
        sqlitePath[sizeof(sqlitePath) - 1] = '\0';
    } else if (type == DatabaseType::POSTGRESQL) {
        const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(db);
        const auto& connInfo = pgDb->getConnectionInfo();
        strncpy(host, connInfo.host.c_str(), sizeof(host) - 1);
        port = connInfo.port;
        strncpy(database, connInfo.database.c_str(), sizeof(database) - 1);
        strncpy(username, connInfo.username.c_str(), sizeof(username) - 1);
        strncpy(password, connInfo.password.c_str(), sizeof(password) - 1);
        showAllDatabases = connInfo.showAllDatabases;
        authType = connInfo.username.empty() ? AUTH_NONE : AUTH_USERNAME_PASSWORD;
        // Map sslmode string to index
        static const char* sslModes[] = {"disable", "allow",     "prefer",
                                         "require", "verify-ca", "verify-full"};
        sslModeIndex = 2; // default "prefer"
        for (int i = 0; i < 6; ++i) {
            if (connInfo.sslmode == sslModes[i]) {
                sslModeIndex = i;
                break;
            }
        }
    } else if (type == DatabaseType::MYSQL) {
        const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(db);
        const auto& connInfo = mysqlDb->getConnectionInfo();
        strncpy(host, connInfo.host.c_str(), sizeof(host) - 1);
        port = connInfo.port;
        strncpy(database, connInfo.database.c_str(), sizeof(database) - 1);
        strncpy(username, connInfo.username.c_str(), sizeof(username) - 1);
        strncpy(password, connInfo.password.c_str(), sizeof(password) - 1);
        showAllDatabases = connInfo.showAllDatabases;
        authType = connInfo.username.empty() ? AUTH_NONE : AUTH_USERNAME_PASSWORD;
    } else if (type == DatabaseType::MONGODB) {
        const auto mongoDb = std::dynamic_pointer_cast<MongoDBDatabase>(db);
        const auto& connInfo = mongoDb->getConnectionInfo();
        strncpy(host, connInfo.host.c_str(), sizeof(host) - 1);
        port = connInfo.port;
        strncpy(database, connInfo.database.c_str(), sizeof(database) - 1);
        strncpy(username, connInfo.username.c_str(), sizeof(username) - 1);
        strncpy(password, connInfo.password.c_str(), sizeof(password) - 1);
        showAllDatabases = connInfo.showAllDatabases;
        authType = connInfo.username.empty() ? AUTH_NONE : AUTH_USERNAME_PASSWORD;
    } else if (type == DatabaseType::REDIS) {
        const auto redisDb = std::dynamic_pointer_cast<RedisDatabase>(db);
        const auto& connInfo = redisDb->getConnectionInfo();
        strncpy(host, connInfo.host.c_str(), sizeof(host) - 1);
        port = connInfo.port;
        strncpy(username, connInfo.username.c_str(), sizeof(username) - 1);
        strncpy(password, connInfo.password.c_str(), sizeof(password) - 1);
        authType = (connInfo.password.empty() && connInfo.username.empty())
                       ? AUTH_NONE
                       : AUTH_USERNAME_PASSWORD;
    } else {
        Logger::warn(
            std::format("editConnection: Unhandled database type {}", static_cast<int>(type)));
    }

    editingDatabase = db;
    Logger::info(std::format("editConnection complete: currentState={}, editingDatabase={}",
                             static_cast<int>(currentState), editingDatabase ? "set" : "null"));
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createSQLiteDatabase() {
    return FileDialog::openSQLiteFile();
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createSqlDatabase(
    const std::string& defaultDatabase,
    const std::function<std::shared_ptr<DatabaseInterface>(
        const std::string&, const std::string&, int, const std::string&, const std::string&,
        const std::string&, bool)>& factory) {
    if (strlen(connectionName) == 0) {
        return nullptr;
    }

    std::string usernameStr;
    std::string passwordStr;

    if (authType == AUTH_USERNAME_PASSWORD) {
        if (strlen(username) == 0) {
            return nullptr;
        }
        usernameStr = std::string(username);
        passwordStr = std::string(password);
    }

    std::string databaseStr = strlen(database) > 0 ? std::string(database) : defaultDatabase;

    return factory(std::string(connectionName), std::string(host), port, databaseStr, usernameStr,
                   passwordStr, showAllDatabases);
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createPostgreSQLDatabase() {
    if (strlen(connectionName) == 0) {
        return nullptr;
    }

    static const char* sslModes[] = {"disable", "allow",     "prefer",
                                     "require", "verify-ca", "verify-full"};

    DatabaseConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.name = std::string(connectionName);
    info.host = std::string(host);
    info.port = port;
    info.database = strlen(database) > 0 ? std::string(database) : "postgres";
    info.showAllDatabases = showAllDatabases;
    info.sslmode = sslModes[sslModeIndex];

    if (authType == AUTH_USERNAME_PASSWORD) {
        if (strlen(username) == 0) {
            return nullptr;
        }
        info.username = std::string(username);
        info.password = std::string(password);
    }

    return std::make_shared<PostgresDatabase>(info);
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createMySQLDatabase() {
    return createSqlDatabase("mysql", [](const std::string& name, const std::string& hostValue,
                                         int portValue, const std::string& databaseValue,
                                         const std::string& usernameValue,
                                         const std::string& passwordValue, bool showAll) {
        DatabaseConnectionInfo info;
        info.type = DatabaseType::MYSQL;
        info.name = name;
        info.host = hostValue;
        info.port = portValue;
        info.database = databaseValue;
        info.username = usernameValue;
        info.password = passwordValue;
        info.showAllDatabases = showAll;
        return std::make_shared<MySQLDatabase>(info);
    });
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createMongoDBDatabase() {
    if (strlen(connectionName) == 0) {
        return nullptr;
    }

    std::string usernameStr;
    std::string passwordStr;

    if (authType == AUTH_USERNAME_PASSWORD) {
        usernameStr = std::string(username);
        passwordStr = std::string(password);
    }

    DatabaseConnectionInfo info;
    info.type = DatabaseType::MONGODB;
    info.name = std::string(connectionName);
    info.host = std::string(host);
    info.port = port;
    info.database = std::string(database);
    info.username = usernameStr;
    info.password = passwordStr;
    info.showAllDatabases = showAllDatabases;

    return std::make_shared<MongoDBDatabase>(info);
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createRedisDatabase() {
    if (strlen(connectionName) == 0) {
        std::cout << "Redis connection failed: Connection name is empty" << std::endl;
        return nullptr;
    }

    std::cout << "Creating RedisDatabase: " << connectionName << " -> " << host << ":" << port
              << " (auth: " << (authType == AUTH_USERNAME_PASSWORD ? "username & password" : "none")
              << ")" << std::endl;
    if (authType == AUTH_USERNAME_PASSWORD && strlen(username) > 0) {
        std::cout << "Using username: " << username << std::endl;
    }

    DatabaseConnectionInfo info;
    info.type = DatabaseType::REDIS;
    info.name = std::string(connectionName);
    info.host = std::string(host);
    info.port = port;
    info.username = (authType == AUTH_USERNAME_PASSWORD) ? std::string(username) : "";
    info.password = (authType == AUTH_USERNAME_PASSWORD) ? std::string(password) : "";

    return std::make_shared<RedisDatabase>(info);
}

void DatabaseConnectionDialog::startAsyncConnection() {
    // Clear previous error
    errorMessage.clear();
    isConnecting = true;

    // If in edit mode, update the saved connection immediately
    if (editingDatabase && editingConnectionId != -1) {
        const auto& app = Application::getInstance();

        // Build updated connection info
        DatabaseConnectionInfo updatedInfo;
        updatedInfo.name = std::string(connectionName);
        updatedInfo.type = selectedDatabaseType;
        updatedInfo.host = std::string(host);
        updatedInfo.port = port;
        updatedInfo.database = std::string(database);
        updatedInfo.username = std::string(username);
        updatedInfo.password = std::string(password);
        updatedInfo.showAllDatabases = showAllDatabases;
        if (selectedDatabaseType == DatabaseType::POSTGRESQL) {
            static const char* sslModes[] = {"disable", "allow",     "prefer",
                                             "require", "verify-ca", "verify-full"};
            updatedInfo.sslmode = sslModes[sslModeIndex];
        }

        // Update the database object with new connection info
        auto const type = editingDatabase->getConnectionInfo().type;
        if (type == DatabaseType::POSTGRESQL) {
            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(editingDatabase);
            if (pgDb) {
                pgDb->setConnectionInfo(updatedInfo);
            }
        } else if (type == DatabaseType::MYSQL) {
            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(editingDatabase);
            if (mysqlDb) {
                mysqlDb->setConnectionInfo(updatedInfo);
            }
        } else if (type == DatabaseType::MONGODB) {
            auto mongoDb = std::dynamic_pointer_cast<MongoDBDatabase>(editingDatabase);
            if (mongoDb) {
                mongoDb->setConnectionInfo(updatedInfo);
            }
        }

        // Update the saved connection in database
        SavedConnection updatedConn;
        updatedConn.id = editingConnectionId;
        updatedConn.connectionInfo = updatedInfo;
        updatedConn.workspaceId = app.getCurrentWorkspaceId();
        app.getAppState()->updateConnection(updatedConn);
    }

    // Start connection using AsyncOperation
    connectionOp.startCancellable([this](std::stop_token stopToken) {
        try {
            if (stopToken.stop_requested()) {
                return std::make_pair(std::shared_ptr<DatabaseInterface>(nullptr),
                                      std::string("Connection cancelled"));
            }
            // Try to create and connect to database based on current state
            std::shared_ptr<DatabaseInterface> db;

            switch (selectedDatabaseType) {
            case DatabaseType::POSTGRESQL:
                db = createPostgreSQLDatabase();
                break;
            case DatabaseType::MYSQL:
                db = createMySQLDatabase();
                break;
            case DatabaseType::MONGODB:
                db = createMongoDBDatabase();
                break;
            case DatabaseType::REDIS:
                std::cout << "Creating Redis database connection..." << std::endl;
                db = createRedisDatabase();
                break;
            case DatabaseType::SQLITE:
                // SQLite is handled synchronously, not here
                break;
            }

            if (stopToken.stop_requested()) {
                return std::make_pair(std::shared_ptr<DatabaseInterface>(nullptr),
                                      std::string("Connection cancelled"));
            }

            if (db) {
                auto [success, error] = db->connect();
                if (stopToken.stop_requested()) {
                    db->disconnect();
                    return std::make_pair(std::shared_ptr<DatabaseInterface>(nullptr),
                                          std::string("Connection cancelled"));
                }
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
    connectionOp.check(
        [this](std::pair<std::shared_ptr<DatabaseInterface>, std::string> result_pair) {
            auto [db, error] = result_pair;

            if (!db) {
                isConnecting = false;
                errorMessage = error;
                return;
            }

            if (!editingDatabase) {
                // Save successful connection for new connections
                SavedConnection conn;
                conn.connectionInfo.name = std::string(connectionName);
                conn.connectionInfo.type = selectedDatabaseType;
                conn.connectionInfo.host = std::string(host);
                conn.connectionInfo.port = port;
                conn.connectionInfo.database = std::string(database);
                conn.connectionInfo.username = std::string(username);
                conn.connectionInfo.password = std::string(password);
                conn.connectionInfo.showAllDatabases = showAllDatabases;
                conn.workspaceId = Application::getInstance().getCurrentWorkspaceId();

                const auto& app = Application::getInstance();
                int newConnectionId = app.getAppState()->saveConnection(conn);
                if (newConnectionId != -1) {
                    db->setConnectionId(newConnectionId);
                }

                result = db;
            } else {
                auto& app = Application::getInstance();

                if (editingConnectionId != -1) {
                    db->setConnectionId(editingConnectionId);
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
        });
}
