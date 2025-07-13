#include "ui/db_connection_dialog.hpp"
#include "database/postgresql.hpp"
#include "utils/file_dialog.hpp"
#include <imgui.h>
#include <iostream>

void DatabaseConnectionDialog::showDialog() {
    if (!isOpen) {
        isOpen = true;
        showingTypeSelection = true;
        showingPostgreSQLConnection = false;
        result = nullptr;
        ImGui::OpenPopup("Connect to Database");
    }

    if (showingTypeSelection) {
        renderTypeSelection();
    } else if (showingPostgreSQLConnection) {
        renderPostgreSQLConnection();
    }
}

void DatabaseConnectionDialog::renderTypeSelection() {
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(350, 200), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Choose the type of database to connect to:");
        ImGui::Separator();
        ImGui::Spacing();

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
                result = createSQLiteDatabase();
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
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400, 350), ImGuiCond_Appearing);

    if (ImGui::BeginPopupModal("Connect to Database", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter PostgreSQL connection details:");
        ImGui::Separator();
        ImGui::Spacing();

        // Add visual styling for input fields
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.4f, 0.4f, 0.4f, 0.8f));

        ImGui::InputText("Connection Name", connectionName, sizeof(connectionName));
        ImGui::InputText("Host", host, sizeof(host));
        ImGui::InputInt("Port", &port);
        ImGui::InputText("Database", database, sizeof(database));
        ImGui::InputText("Username", username, sizeof(username));
        ImGui::InputText("Password", password, sizeof(password), ImGuiInputTextFlags_Password);

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

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
    isConnecting = false;
    errorMessage.clear();
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
