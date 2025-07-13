#include "ui/db_connection_dialog.hpp"
#include "application.hpp"
#include "database/postgresql.hpp"
#include "database/sqlite.hpp"
#include "utils/file_dialog.hpp"
#include <imgui.h>
#include <iostream>
#include <themes.hpp>

void DatabaseConnectionDialog::showDialog() {
    if (!isOpen) {
        isOpen = true;
        showingTypeSelection = true;
        showingPostgreSQLConnection = false;
        showingSavedConnections = false;
        result = nullptr;
        loadSavedConnections();
        ImGui::OpenPopup("Connect to Database");
    }

    if (showingTypeSelection) {
        renderTypeSelection();
    } else if (showingPostgreSQLConnection) {
        renderPostgreSQLConnection();
    } else if (showingSavedConnections) {
        renderSavedConnections();
    }
}

void DatabaseConnectionDialog::renderTypeSelection() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(300, 360), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Choose how to connect to a database:");
        ImGui::Separator();
        ImGui::Spacing();

        if (!savedConnections.empty()) {
            if (ImGui::Button("Saved Connections", ImVec2(-1, 30))) {
                showingTypeSelection = false;
                showingSavedConnections = true;
            }
            ImGui::Spacing();
            ImGui::Text("Or create a new connection:");
            ImGui::Spacing();
        }

        ImGui::RadioButton("SQLite File", &selectedDatabaseType, 0);
        ImGui::Text("   Open a local SQLite database file");
        ImGui::Spacing();

        ImGui::RadioButton("PostgreSQL Server", &selectedDatabaseType, 1);
        ImGui::Text("   Connect to a PostgreSQL server");
        ImGui::Spacing();

        ImGui::Separator();

        if (ImGui::Button("Next", ImVec2(100, 0))) {
            if (selectedDatabaseType == 0) {
                // SQLite - directly open file dialog
                auto db = createSQLiteDatabase();
                if (db) {
                    // Save SQLite connection to app state
                    SavedConnection conn;
                    conn.name = db->getName();
                    conn.type = "sqlite";
                    conn.path = db->getPath();

                    auto &app = Application::getInstance();
                    app.getAppState()->saveConnection(conn);

                    result = db;
                }
                ImGui::CloseCurrentPopup();
                reset();
            } else {
                // PostgreSQL - show connection dialog
                showingTypeSelection = false;
                showingPostgreSQLConnection = true;
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

void DatabaseConnectionDialog::renderPostgreSQLConnection() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 360), ImGuiCond_Always);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter PostgreSQL connection details:");
        ImGui::Separator();
        ImGui::Spacing();

        // Add visual styling for input fields using Theme colors
        const auto &colors =
            Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay1);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, colors.surface1);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, colors.surface2);

        ImGui::InputText("Connection Name", connectionName, sizeof(connectionName));
        ImGui::InputText("Host", host, sizeof(host));
        ImGui::InputInt("Port", &port);
        ImGui::InputText("Database", database, sizeof(database));
        ImGui::InputText("Username", username, sizeof(username));
        ImGui::InputText("Password", password, sizeof(password), ImGuiInputTextFlags_Password);

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Checkbox("Show all databases from server", &showAllDatabases);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("When checked, shows all databases from the server in the sidebar.\nWhen "
                        "unchecked, only shows the specified database.");
            ImGui::EndTooltip();
        }

        ImGui::Spacing();

        // Show error message if there is one
        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        ImGui::Separator();

        // Show loading spinner or connect button
        if (isConnecting) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.3f, 0.3f, 1.0f));
            ImGui::Button("Connecting...", ImVec2(100, 0));
            ImGui::PopStyleColor();

            // Simple spinner animation
            ImGui::SameLine();
            ImGui::Text("%c", "|/-\\"[(int)(ImGui::GetTime() / 0.1f) & 3]);
        } else {
            if (ImGui::Button("Connect", ImVec2(100, 0))) {
                // Clear previous error
                errorMessage.clear();
                isConnecting = true;

                // Try to create and connect to database
                auto db = createPostgreSQLDatabase();
                if (db) {
                    auto [success, error] = db->connect();
                    if (success) {
                        // Save successful connection
                        SavedConnection conn;
                        conn.name = std::string(connectionName);
                        conn.type = "postgresql";
                        conn.host = std::string(host);
                        conn.port = port;
                        conn.database = std::string(database);
                        conn.username = std::string(username);

                        auto &app = Application::getInstance();
                        app.getAppState()->saveConnection(conn);

                        result = db;
                        ImGui::CloseCurrentPopup();
                        reset();
                    } else {
                        isConnecting = false;
                        errorMessage = "Failed to connect: " + error;
                    }
                } else {
                    isConnecting = false;
                    errorMessage = "Please fill in all required fields";
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            showingPostgreSQLConnection = false;
            showingTypeSelection = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            ImGui::CloseCurrentPopup();
            reset();
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
    showingTypeSelection = false;
    showingPostgreSQLConnection = false;
    showingSavedConnections = false;
    isConnecting = false;
    errorMessage.clear();
    selectedSavedConnection = -1;
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createSQLiteDatabase() {
    return FileDialog::openSQLiteFile();
}

std::shared_ptr<DatabaseInterface> DatabaseConnectionDialog::createPostgreSQLDatabase() {
    if (strlen(connectionName) == 0 || strlen(database) == 0 || strlen(username) == 0) {
        return nullptr;
    }

    return std::make_shared<PostgreSQLDatabase>(std::string(connectionName), std::string(host),
                                                port, std::string(database), std::string(username),
                                                std::string(password));
}

void DatabaseConnectionDialog::loadSavedConnections() {
    auto &app = Application::getInstance();
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
                const auto &conn = savedConnections[i];

                bool isSelected = (selectedSavedConnection == (int)i);
                if (ImGui::Selectable((conn.name + " (" + conn.type + ")").c_str(), &isSelected)) {
                    selectedSavedConnection = (int)i;
                }

                if (isSelected && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    if (conn.type == "postgresql") {
                        ImGui::Text("Host: %s:%d", conn.host.c_str(), conn.port);
                        ImGui::Text("Database: %s", conn.database.c_str());
                        ImGui::Text("Username: %s", conn.username.c_str());
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
            const auto &conn = savedConnections[selectedSavedConnection];

            if (conn.type == "postgresql") {
                // Fill in the PostgreSQL fields and connect
                strncpy(connectionName, conn.name.c_str(), sizeof(connectionName) - 1);
                strncpy(host, conn.host.c_str(), sizeof(host) - 1);
                port = conn.port;
                strncpy(database, conn.database.c_str(), sizeof(database) - 1);
                strncpy(username, conn.username.c_str(), sizeof(username) - 1);
                password[0] = '\0'; // Clear password for security

                auto db = createPostgreSQLDatabase();
                if (db) {
                    auto [success, error] = db->connect();
                    if (success) {
                        // Update last used timestamp
                        auto &app = Application::getInstance();
                        app.getAppState()->updateLastUsed(conn.id);

                        result = db;
                        ImGui::CloseCurrentPopup();
                        reset();
                    } else {
                        errorMessage = "Failed to connect: " + error;
                    }
                }
            } else if (conn.type == "sqlite") {
                // Create SQLite database from saved path
                auto db = std::make_shared<SQLiteDatabase>(conn.name, conn.path);
                if (db) {
                    auto [success, error] = db->connect();
                    if (success) {
                        // Update last used timestamp
                        auto &app = Application::getInstance();
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
            auto &app = Application::getInstance();
            app.getAppState()->deleteConnection(savedConnections[selectedSavedConnection].id);
            loadSavedConnections(); // Refresh list
            selectedSavedConnection = -1;
        }

        ImGui::SameLine();
        if (ImGui::Button("Back", ImVec2(100, 0))) {
            showingSavedConnections = false;
            showingTypeSelection = true;
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
            ImGui::PopStyleColor();
        }

        ImGui::EndPopup();
    }
}
