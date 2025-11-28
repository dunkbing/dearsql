#include "ui/rename_dialog.hpp"
#include "application.hpp"
#include "imgui.h"
#include "themes.hpp"
#include "utils/logger.hpp"
#include <cstring>
#include <format>

RenameDialog& RenameDialog::instance() {
    static RenameDialog instance;
    return instance;
}

void RenameDialog::show(PostgresDatabaseNode* dbNode, RefreshCallback onSuccess) {
    reset();
    targetNode = dbNode;
    targetName = dbNode->name;
    currentObjectType = ObjectType::Database;
    successCallback = std::move(onSuccess);
    std::strncpy(newNameBuffer, targetName.c_str(), sizeof(newNameBuffer) - 1);
    isDialogOpen = true;
}

void RenameDialog::show(PostgresSchemaNode* schemaNode, RefreshCallback onSuccess) {
    reset();
    targetNode = schemaNode;
    targetName = schemaNode->name;
    currentObjectType = ObjectType::Schema;
    successCallback = std::move(onSuccess);
    std::strncpy(newNameBuffer, targetName.c_str(), sizeof(newNameBuffer) - 1);
    isDialogOpen = true;
}

void RenameDialog::show(MySQLDatabaseNode* dbNode, RefreshCallback onSuccess) {
    reset();
    targetNode = dbNode;
    targetName = dbNode->name;
    currentObjectType = ObjectType::Database;
    successCallback = std::move(onSuccess);
    std::strncpy(newNameBuffer, targetName.c_str(), sizeof(newNameBuffer) - 1);
    isDialogOpen = true;
}

void RenameDialog::showForTable(PostgresSchemaNode* schemaNode, const std::string& tableName,
                                RefreshCallback onSuccess) {
    reset();
    targetNode = schemaNode;
    targetName = tableName;
    currentObjectType = ObjectType::Table;
    successCallback = std::move(onSuccess);
    std::strncpy(newNameBuffer, targetName.c_str(), sizeof(newNameBuffer) - 1);
    isDialogOpen = true;
}

void RenameDialog::showForTable(MySQLDatabaseNode* dbNode, const std::string& tableName,
                                RefreshCallback onSuccess) {
    reset();
    targetNode = dbNode;
    targetName = tableName;
    currentObjectType = ObjectType::Table;
    successCallback = std::move(onSuccess);
    std::strncpy(newNameBuffer, targetName.c_str(), sizeof(newNameBuffer) - 1);
    isDialogOpen = true;
}

void RenameDialog::showForTable(SQLiteDatabase* sqliteDb, const std::string& tableName,
                                RefreshCallback onSuccess) {
    reset();
    targetNode = sqliteDb;
    targetName = tableName;
    currentObjectType = ObjectType::Table;
    successCallback = std::move(onSuccess);
    std::strncpy(newNameBuffer, targetName.c_str(), sizeof(newNameBuffer) - 1);
    isDialogOpen = true;
}

void RenameDialog::render() {
    if (!isDialogOpen)
        return;

    const std::string title = std::format("Rename {}", getObjectTypeDisplayName());

    if (!ImGui::IsPopupOpen(title.c_str())) {
        ImGui::OpenPopup(title.c_str());
    }

    ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);

    if (ImGui::BeginPopupModal(title.c_str(), &isDialogOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto& theme = Application::getInstance().getCurrentColors();

        ImGui::Text("Current name: %s", targetName.c_str());
        ImGui::Spacing();

        ImGui::Text("New name:");
        ImGui::SetNextItemWidth(-1);

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        const bool enterPressed =
            ImGui::InputText("##new_name", newNameBuffer, sizeof(newNameBuffer),
                             ImGuiInputTextFlags_EnterReturnsTrue);

        if (!errorMessage.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, theme.red);
            ImGui::TextWrapped("%s", errorMessage.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool canRename =
            std::strlen(newNameBuffer) > 0 && std::string(newNameBuffer) != targetName;

        if (!canRename) {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Rename", ImVec2(100, 0)) || (enterPressed && canRename)) {
            if (executeRename()) {
                if (successCallback) {
                    successCallback();
                }
                isDialogOpen = false;
            }
        }

        if (!canRename) {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            isDialogOpen = false;
        }

        ImGui::EndPopup();
    }

    if (!isDialogOpen) {
        reset();
    }
}

bool RenameDialog::executeRename() {
    try {
        const std::string sql = generateRenameSQL();
        if (sql.empty()) {
            return false;
        }

        Logger::info("Executing: " + sql);

        IQueryExecutor* executor = nullptr;

        if (auto* pgSchema = std::get_if<PostgresSchemaNode*>(&targetNode)) {
            executor = *pgSchema;
        } else if (auto* pgDb = std::get_if<PostgresDatabaseNode*>(&targetNode)) {
            executor = *pgDb;
        } else if (auto* mysqlDb = std::get_if<MySQLDatabaseNode*>(&targetNode)) {
            executor = *mysqlDb;
        } else if (auto* sqliteDb = std::get_if<SQLiteDatabase*>(&targetNode)) {
            executor = *sqliteDb;
        }

        if (!executor) {
            errorMessage = "Unable to execute query on this database type";
            return false;
        }

        const auto result = executor->executeQueryWithResult(sql);
        if (!result.success) {
            errorMessage = result.errorMessage;
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        errorMessage = std::format("Failed to rename: {}", e.what());
        Logger::error(errorMessage);
        return false;
    }
}

std::string RenameDialog::generateRenameSQL() {
    const std::string newName = newNameBuffer;

    if (currentObjectType == ObjectType::Database) {
        if (std::holds_alternative<PostgresDatabaseNode*>(targetNode)) {
            return std::format("ALTER DATABASE \"{}\" RENAME TO \"{}\"", targetName, newName);
        } else if (std::holds_alternative<MySQLDatabaseNode*>(targetNode)) {
            errorMessage = "MySQL does not support direct database renaming. "
                           "You need to create a new database, copy all data, and drop the old "
                           "one.";
            return "";
        }
    } else if (currentObjectType == ObjectType::Schema) {
        if (std::holds_alternative<PostgresSchemaNode*>(targetNode)) {
            return std::format("ALTER SCHEMA \"{}\" RENAME TO \"{}\"", targetName, newName);
        }
    } else if (currentObjectType == ObjectType::Table) {
        if (auto* pgSchemaPtr = std::get_if<PostgresSchemaNode*>(&targetNode)) {
            return std::format("ALTER TABLE \"{}\".\"{}\" RENAME TO \"{}\"", (*pgSchemaPtr)->name,
                               targetName, newName);
        } else if (std::holds_alternative<MySQLDatabaseNode*>(targetNode)) {
            return std::format("RENAME TABLE `{}` TO `{}`", targetName, newName);
        } else if (std::holds_alternative<SQLiteDatabase*>(targetNode)) {
            return std::format("ALTER TABLE \"{}\" RENAME TO \"{}\"", targetName, newName);
        }
    }

    return "";
}

std::string RenameDialog::getObjectTypeDisplayName() const {
    switch (currentObjectType) {
    case ObjectType::Database:
        return "Database";
    case ObjectType::Schema:
        return "Schema";
    case ObjectType::Table:
        return "Table";
    default:
        return "Object";
    }
}

void RenameDialog::reset() {
    targetNode = std::monostate{};
    targetName.clear();
    std::memset(newNameBuffer, 0, sizeof(newNameBuffer));
    errorMessage.clear();
    successCallback = nullptr;
}
