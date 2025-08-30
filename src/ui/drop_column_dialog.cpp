#include "ui/drop_column_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "ui/log_panel.hpp"

void DropColumnDialog::showDropColumnDialog(const std::shared_ptr<DatabaseInterface>& db,
                                            const std::string& tableName,
                                            const std::string& columnName) {
    database = db;
    targetTableName = tableName;
    targetColumnName = columnName;

    isOpen = true;
    hasCompletedResult = false;
    errorMessage.clear();
}

void DropColumnDialog::renderDialog() {
    if (!isOpen)
        return;

    const auto title = "Drop Column";

    // Always try to open the popup when dialog is active
    if (!ImGui::IsPopupOpen(title)) {
        ImGui::OpenPopup(title);
    }

    // Set popup size
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal(title, &isOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Warning icon and message
        const auto& theme =
            Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
        ImGui::PushStyleColor(ImGuiCol_Text, theme.peach); // Warning color
        ImGui::Text("⚠️ Warning: This action cannot be undone!");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Show details
        ImGui::Text("You are about to drop the following column:");
        ImGui::Spacing();

        ImGui::Indent();
        ImGui::Text("Table: %s", targetTableName.c_str());
        ImGui::Text("Column: %s", targetColumnName.c_str());
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::Text("This will:");
        ImGui::BulletText("Permanently delete the column and all its data");
        ImGui::BulletText("Remove any indexes or constraints on this column");
        ImGui::BulletText("Potentially break applications that depend on this column");

        ImGui::Spacing();
        ImGui::Separator();

        // Show error message if any
        if (!errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
            ImGui::TextWrapped("Error: %s", errorMessage.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }

        // Buttons
        ImGui::PushStyleColor(ImGuiCol_Button, theme.red);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.maroon);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(theme.red.x * 0.8f, theme.red.y * 0.8f,
                                                            theme.red.z * 0.8f, 1.0f));

        if (ImGui::Button("Drop Column", ImVec2(120, 0))) {
            if (executeDropColumn()) {
                hasCompletedResult = true;
                isOpen = false;
            }
        }

        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            isOpen = false;
        }

        ImGui::EndPopup();
    }

    if (!isOpen) {
        // Dialog was closed, reset state
        errorMessage.clear();
    }
}

bool DropColumnDialog::executeDropColumn() {
    try {
        const std::string sql = generateDropColumnSQL();
        if (sql.empty()) {
            return false;
        }

        LogPanel::info("Executing: " + sql);

        const std::string result = database->executeQuery(sql);

        // Check if there was an error in the result
        if (result.find("ERROR") != std::string::npos ||
            result.find("Error") != std::string::npos) {
            errorMessage = "Failed to drop column: " + result;
            return false;
        }

        // Refresh table structure
        database->setTablesLoaded(false);
        database->refreshTables();

        return true;

    } catch (const std::exception& e) {
        errorMessage = "Failed to drop column: " + std::string(e.what());
        LogPanel::error(errorMessage);
        return false;
    }
}

std::string DropColumnDialog::generateDropColumnSQL() {
    std::string sql;

    switch (database->getType()) {
    case DatabaseType::POSTGRESQL:
    case DatabaseType::MYSQL:
        sql = "ALTER TABLE " + targetTableName + " DROP COLUMN " + targetColumnName;
        break;

    case DatabaseType::SQLITE:
        // SQLite doesn't support DROP COLUMN directly (before version 3.35.0)
        errorMessage =
            "SQLite doesn't support dropping columns directly. You need to recreate the table.";
        return "";

    default:
        errorMessage = "Column dropping not supported for this database type";
        return "";
    }

    return sql;
}
