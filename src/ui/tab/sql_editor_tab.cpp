#include "ui/tab/sql_editor_tab.hpp"
#include "database/db.hpp"
#include "database/mysql.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "database/sqlite.hpp"
#include "imgui.h"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <ranges>
#include <set>
#include <variant>

// Constructor for PostgreSQL database node
SQLEditorTab::SQLEditorTab(const std::string& name, PostgresDatabaseNode* dbNode)
    : Tab(name, TabType::SQL_EDITOR), databaseNode(dbNode) {
    sqlEditor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);

    // Populate auto-complete with table and column names
    populateAutoCompleteKeywords();

    // Start loading schemas if not already loaded
    if (dbNode && !dbNode->schemasLoaded && !dbNode->schemasLoader.isRunning()) {
        dbNode->startSchemasLoadAsync();
    }
}

// Constructor for MySQL database node
SQLEditorTab::SQLEditorTab(const std::string& name, MySQLDatabaseNode* dbNode)
    : Tab(name, TabType::SQL_EDITOR), databaseNode(dbNode) {
    sqlEditor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);

    // Populate auto-complete with table and column names
    populateAutoCompleteKeywords();
}

SQLEditorTab::SQLEditorTab(const std::string& name, SQLiteDatabase* dbNode)
    : Tab(name, TabType::SQL_EDITOR), databaseNode(dbNode) {
    sqlEditor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);

    // Populate auto-complete with table and column names
    populateAutoCompleteKeywords();
}

SQLEditorTab::~SQLEditorTab() {
    // Wait for any ongoing query execution to complete
    if (isExecutingQuery && queryExecutionFuture.valid()) {
        queryExecutionFuture.wait();
    }
}

void SQLEditorTab::render() {
    checkQueryExecutionStatus();

    renderConnectionInfo();

    totalContentHeight = ImGui::GetContentRegionAvail().y;
    const float editorHeight = totalContentHeight * splitterPosition;
    const float resultsHeight =
        totalContentHeight * (1.0f - splitterPosition) - 6.0f; // 6px hover area for splitter

    if (ImGui::BeginChild("SQLEditor", ImVec2(-1, editorHeight), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        sqlEditor.Render("##SQL", true, ImVec2(-1, -1), true);
        sqlQuery = sqlEditor.GetText();
    }
    ImGui::EndChild();

    renderVerticalSplitter("##sql_splitter", &splitterPosition, 100.0f, 200.0f);

    if (ImGui::BeginChild("SQLResults", ImVec2(-1, resultsHeight), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        renderToolbar();
        renderDatabaseSchemaSelector();
        renderQueryResults();
    }
    ImGui::EndChild();
}

void SQLEditorTab::renderConnectionInfo() {
    if (std::holds_alternative<PostgresDatabaseNode*>(databaseNode)) {
        auto* pgNode = std::get<PostgresDatabaseNode*>(databaseNode);
        if (pgNode && pgNode->parentDb) {
            if (!selectedSchemaName.empty()) {
                ImGui::Text("Server: %s | Database: %s | Schema: %s",
                            pgNode->parentDb->getConnectionInfo().name.c_str(),
                            pgNode->name.c_str(), selectedSchemaName.c_str());
            } else {
                ImGui::Text("Server: %s | Database: %s",
                            pgNode->parentDb->getConnectionInfo().name.c_str(),
                            pgNode->name.c_str());
            }
        } else {
            ImGui::Text("SQL Editor (No database selected)");
        }
    } else if (std::holds_alternative<MySQLDatabaseNode*>(databaseNode)) {
        auto* mysqlNode = std::get<MySQLDatabaseNode*>(databaseNode);
        if (mysqlNode && mysqlNode->parentDb) {
            ImGui::Text("Server: %s | Database: %s",
                        mysqlNode->parentDb->getConnectionInfo().name.c_str(),
                        mysqlNode->name.c_str());
        } else {
            ImGui::Text("SQL Editor (No database selected)");
        }
    } else if (std::holds_alternative<SQLiteDatabase*>(databaseNode)) {
        auto* sqliteNode = std::get<SQLiteDatabase*>(databaseNode);
        if (sqliteNode) {
            ImGui::Text("Database: %s", sqliteNode->getConnectionInfo().name.c_str());
        } else {
            ImGui::Text("SQL Editor (No database selected)");
        }
    } else {
        ImGui::Text("SQL Editor (No database selected)");
    }
    ImGui::Separator();
}

void SQLEditorTab::renderToolbar() {
    if (isExecutingQuery) {
        ImGui::BeginDisabled();
        ImGui::Button("Executing...");
        ImGui::EndDisabled();

        ImGui::SameLine();
        UIUtils::Spinner("##query_spinner", 8.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));

        ImGui::SameLine();
        ImGui::Text("Running query...");

        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            cancelQueryExecution();
        }
    } else {
        if (ImGui::Button("Execute Query")) {
            startQueryExecutionAsync(sqlQuery);
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        sqlEditor.SetText("");
        sqlQuery.clear();
        hasStructuredResults = false;
        queryColumnNames.clear();
        queryTableData.clear();
        queryError.clear();
    }
}

void SQLEditorTab::renderDatabaseSchemaSelector() {
    ImGui::SameLine();
    const bool isMySQL = std::holds_alternative<MySQLDatabaseNode*>(databaseNode);
    ImGui::Text(isMySQL ? "Database:" : "Schema:");
    ImGui::SameLine();

    ImGui::SetNextItemWidth(200.0f);

    if (std::holds_alternative<MySQLDatabaseNode*>(databaseNode)) {
        renderMySQLDatabaseSelector();
    } else if (std::holds_alternative<PostgresDatabaseNode*>(databaseNode)) {
        renderPostgresSchemaSelector();
    } else if (std::holds_alternative<SQLiteDatabase*>(databaseNode)) {
    } else {
        renderSchemaSelectorForDisconnected();
    }

    ImGui::Separator();
}

void SQLEditorTab::renderSchemaSelectorForDisconnected() {
    const std::string currentSelection = selectedSchemaName.empty() ? "None" : selectedSchemaName;
    if (ImGui::BeginCombo("##schema_combo", currentSelection.c_str())) {
        if (ImGui::Selectable("None", true)) {
            databaseNode.emplace<std::monostate>();
            selectedSchemaName.clear();
        }
        ImGui::EndCombo();
    }
}

void SQLEditorTab::renderLoadingSchemaCombo() {
    ImGui::BeginDisabled();
    if (ImGui::BeginCombo("##schema_combo", "Loading...")) {
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    UIUtils::Spinner("##schema_loading_spinner", 8.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
}

void SQLEditorTab::renderMySQLDatabaseSelector() {
    const MySQLDatabaseNode* currentNode = std::get<MySQLDatabaseNode*>(databaseNode);

    MySQLDatabase* mysqlDb = currentNode ? currentNode->parentDb : nullptr;

    std::string currentSelection = "None";
    if (currentNode && !currentNode->name.empty()) {
        currentSelection = currentNode->name;
    }

    const bool loadingDatabases = mysqlDb && mysqlDb->isLoadingDatabases();
    if (loadingDatabases) {
        renderLoadingSchemaCombo();
        return;
    }

    if (ImGui::BeginCombo("##schema_combo", currentSelection.c_str())) {
        const bool noSelection = currentNode == nullptr || currentNode->name.empty();
        if (ImGui::Selectable("None", noSelection)) {
            databaseNode.emplace<std::monostate>();
            selectedSchemaName.clear();
        }

        if (mysqlDb) {
            auto& databaseDataMap = mysqlDb->getDatabaseDataMap();
            for (const auto& dataPtr : databaseDataMap | std::views::values) {
                if (!dataPtr) {
                    continue;
                }

                auto* dbNode = dataPtr.get();
                const bool isSelected = currentNode && currentNode->name == dbNode->name;
                if (ImGui::Selectable(dbNode->name.c_str(), isSelected)) {
                    databaseNode = dbNode;
                    selectedSchemaName.clear();
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }

            if (mysqlDb->isLoadingDatabases()) {
                mysqlDb->checkDatabasesStatusAsync();
                ImGui::Text("  Loading databases...");
            }
        }

        ImGui::EndCombo();
    }
}

void SQLEditorTab::renderPostgresSchemaSelector() {
    PostgresDatabaseNode* currentNode = std::get<PostgresDatabaseNode*>(databaseNode);

    PostgresDatabase* pgDb = currentNode ? currentNode->parentDb : nullptr;

    std::string currentSelection = "None";
    if (currentNode && !selectedSchemaName.empty()) {
        currentSelection = std::format("{}.{}", currentNode->name, selectedSchemaName);
    }

    std::vector<PostgresDatabaseNode*> availableDatabases;
    bool isLoadingAnySchemas = false;
    bool needsSchemasForTargetDb = false;

    if (pgDb) {
        const auto& databaseDataMap = pgDb->getDatabaseDataMap();
        availableDatabases.reserve(databaseDataMap.size());
        for (const auto& db : databaseDataMap | std::views::values) {
            if (db) {
                availableDatabases.push_back(db.get());
            }
        }

        std::string targetDb;
        if (currentNode && !currentNode->name.empty()) {
            targetDb = currentNode->name;
        } else {
            targetDb = pgDb->getConnectionInfo().database;
        }

        if (!targetDb.empty()) {
            const auto* dbData = pgDb->getDatabaseData(targetDb);
            if (dbData && dbData->schemasLoader.isRunning()) {
                isLoadingAnySchemas = true;
            }

            if (dbData && selectedSchemaName.empty() && !dbData->schemasLoaded &&
                !dbData->schemasLoader.isRunning()) {
                needsSchemasForTargetDb = true;
            }
        }

        // if (!targetDb.empty() && targetDb == pgDb->getConnectionInfo().database) {
        //     isLoadingAnySchemas = true;
        // }

        if (pgDb->isLoadingDatabases()) {
            isLoadingAnySchemas = true;
        }
    }

    if (!pgDb) {
        renderSchemaSelectorForDisconnected();
        return;
    }

    if (isLoadingAnySchemas || needsSchemasForTargetDb) {
        renderLoadingSchemaCombo();
        return;
    }

    if (ImGui::BeginCombo("##schema_combo", currentSelection.c_str())) {
        const bool noSchemaSelected = selectedSchemaName.empty();
        if (ImGui::Selectable("None", noSchemaSelected)) {
            databaseNode.emplace<std::monostate>();
            selectedSchemaName.clear();
        }

        if (pgDb->getConnectionInfo().showAllDatabases) {
            const auto& databaseDataMap = pgDb->getDatabaseDataMap();
            for (const auto& dbDataPtr : databaseDataMap | std::views::values) {
                if (dbDataPtr) {
                    dbDataPtr->checkSchemasStatusAsync();
                }
            }

            std::string targetDb = (currentNode && !currentNode->name.empty())
                                       ? currentNode->name
                                       : pgDb->getConnectionInfo().database;

            if (!targetDb.empty()) {
                auto* targetDbData = pgDb->getDatabaseData(targetDb);
                if (targetDbData && !targetDbData->schemasLoaded &&
                    !targetDbData->schemasLoader.isRunning()) {
                    if (targetDb == pgDb->getConnectionInfo().database) {
                        // TODO: load schemas
                    } else {
                        targetDbData->startSchemasLoadAsync();
                    }
                }
            }

            for (const auto& [dbName, dbDataPtr] : databaseDataMap) {
                if (dbDataPtr && !dbDataPtr->schemasLoaded &&
                    !dbDataPtr->schemasLoader.isRunning() &&
                    (!currentNode || dbName != currentNode->name)) {
                    dbDataPtr->startSchemasLoadAsync();
                }
            }
        }

        for (auto* db : availableDatabases) {
            if (!db) {
                continue;
            }

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
            ImGui::Text("%s", db->name.c_str());
            ImGui::PopStyleColor();

            std::vector<std::string> schemas;
            if (db->schemasLoaded) {
                for (const auto& schemaPtr : db->schemas) {
                    if (schemaPtr) {
                        schemas.push_back(schemaPtr->name);
                    }
                }
            } else if (!db->schemasLoader.isRunning()) {
                db->startSchemasLoadAsync();
            }

            for (const auto& schemaName : schemas) {
                ImGui::Indent(16.0f);
                const bool isSelected = (currentNode && currentNode->name == db->name &&
                                         selectedSchemaName == schemaName);
                const std::string schemaLabel = "  " + schemaName;
                const std::string schemaId =
                    std::format("{}##{}_{}", schemaLabel, db->name, schemaName);

                if (ImGui::Selectable(schemaId.c_str(), isSelected)) {
                    databaseNode = db;
                    selectedSchemaName = schemaName;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::Unindent(16.0f);
            }

            const bool isCurrentDatabase = currentNode && currentNode->name == db->name;
            if (db->schemasLoader.isRunning()) {
                ImGui::Indent(16.0f);
                ImGui::Text("  Loading schemas...");
                ImGui::Unindent(16.0f);
            } else if (!isCurrentDatabase && !db->schemasLoaded && db->schemas.empty()) {
                ImGui::Indent(16.0f);
                ImGui::Text("  Click to load schemas...");
                ImGui::Unindent(16.0f);
            }
        }

        ImGui::EndCombo();
    }
}

void SQLEditorTab::renderQueryResults() const {
    if (!queryError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", queryError.c_str());
    } else if (hasStructuredResults && !queryColumnNames.empty()) {
        if (queryTableData.empty()) {
            ImGui::Text("No rows returned.");
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
            }
        } else {
            ImGui::Text("Rows: %zu", queryTableData.size());
            if (queryTableData.size() >= 1000) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(limited to 1000 rows)");
            }
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
            }

            float tableAvailableHeight = ImGui::GetContentRegionAvail().y - 20.0f;
            tableAvailableHeight = std::max(tableAvailableHeight, 50.0f);

            TableRenderer::Config config;
            config.allowEditing = false;
            config.allowSelection = true;
            config.showRowNumbers = false;
            config.minHeight = tableAvailableHeight;

            TableRenderer tableRenderer(config);
            tableRenderer.setColumns(queryColumnNames);
            tableRenderer.setData(queryTableData);

            tableRenderer.render("QueryResults");
        }
    } else if (hasStructuredResults && queryColumnNames.empty()) {
        ImGui::Text("Query executed successfully.");
        if (lastQueryDuration.count() > 0) {
            ImGui::SameLine();
            ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
        }
    } else {
        ImGui::Text("No results to display. Execute a query to see results here.");
    }
}

void SQLEditorTab::startQueryExecutionAsync(const std::string& query) {
    if (isExecutingQuery) {
        return; // Already executing
    }

    isExecutingQuery = true;
    shouldCancelQuery = false;
    hasStructuredResults = false;
    queryError.clear();
    queryColumnNames.clear();
    queryTableData.clear();
    lastQueryDuration = std::chrono::milliseconds{0};

    // start async query execution
    queryExecutionFuture = std::async(std::launch::async, [this, query]() {
        if (shouldCancelQuery) {
            return;
        }

        QueryResult result;

        // execute query based on database type
        if (std::holds_alternative<PostgresDatabaseNode*>(databaseNode)) {
            auto* pgNode = std::get<PostgresDatabaseNode*>(databaseNode);
            if (pgNode) {
                result = pgNode->executeQueryWithResult(query);
            } else {
                result.success = false;
                result.errorMessage = "No database selected";
            }
        } else if (std::holds_alternative<MySQLDatabaseNode*>(databaseNode)) {
            auto* mysqlNode = std::get<MySQLDatabaseNode*>(databaseNode);
            if (mysqlNode) {
                result = mysqlNode->executeQueryWithResult(query);
            } else {
                result.success = false;
                result.errorMessage = "No database selected";
            }
        } else if (std::holds_alternative<SQLiteDatabase*>(databaseNode)) {
            auto* sqliteNode = std::get<SQLiteDatabase*>(databaseNode);
            if (sqliteNode) {
                result = sqliteNode->executeQueryWithResult(query);
            } else {
                result.success = false;
                result.errorMessage = "No database selected";
            }
        } else {
            result.success = false;
            result.errorMessage = "No database selected";
        }

        // check for cancellation before setting results
        if (shouldCancelQuery) {
            return;
        }

        // update UI state with results
        lastQueryDuration = std::chrono::milliseconds{result.executionTimeMs};

        if (result.success) {
            queryColumnNames = result.columnNames;
            queryTableData = result.tableData;
            hasStructuredResults = true;
            queryError.clear();
            queryResult.clear();
        } else {
            queryResult = result.errorMessage;
            queryError = result.errorMessage;
            hasStructuredResults = false;
            queryColumnNames.clear();
            queryTableData.clear();
        }
    });
}

void SQLEditorTab::checkQueryExecutionStatus() {
    if (!isExecutingQuery) {
        return;
    }

    if (queryExecutionFuture.valid() &&
        queryExecutionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            queryExecutionFuture.get();
        } catch (const std::exception& e) {
            if (!shouldCancelQuery) {
                queryResult = "Error in async query execution: " + std::string(e.what());
                queryError = queryResult;
                hasStructuredResults = false;
                queryColumnNames.clear();
                queryTableData.clear();
            }
        }

        isExecutingQuery = false;
    }
}

void SQLEditorTab::cancelQueryExecution() {
    shouldCancelQuery = true;
    // Note: We can't cancel the database query once it's started,
    // but we can prevent the results from being processed
    queryResult = "Query execution cancelled by user";
    queryError = queryResult;
    hasStructuredResults = false;
    queryColumnNames.clear();
    queryTableData.clear();
}

void SQLEditorTab::populateAutoCompleteKeywords() {
    std::set<std::string> uniqueKeywords;

    if (std::holds_alternative<PostgresDatabaseNode*>(databaseNode)) {
        auto* pgNode = std::get<PostgresDatabaseNode*>(databaseNode);
        if (pgNode && pgNode->parentDb) {
            PostgresDatabase* pgDb = pgNode->parentDb;

            // Add tables and columns from all loaded schemas
            if (pgNode->schemasLoaded) {
                for (const auto& schemaPtr : pgNode->schemas) {
                    if (!schemaPtr) {
                        continue;
                    }

                    // Add schema name
                    uniqueKeywords.insert(schemaPtr->name);

                    // Add table names and column names
                    if (schemaPtr->tablesLoaded) {
                        for (const auto& table : schemaPtr->tables) {
                            uniqueKeywords.insert(table.name);
                            for (const auto& column : table.columns) {
                                uniqueKeywords.insert(column.name);
                            }
                        }
                    }

                    // Add view names and column names
                    if (schemaPtr->viewsLoaded) {
                        for (const auto& view : schemaPtr->views) {
                            uniqueKeywords.insert(view.name);
                            for (const auto& column : view.columns) {
                                uniqueKeywords.insert(column.name);
                            }
                        }
                    }
                }
            }

            // Add database names if in multi-database mode
            if (pgDb->getConnectionInfo().showAllDatabases) {
                const auto& databaseDataMap = pgDb->getDatabaseDataMap();
                for (const auto& dbName : databaseDataMap | std::views::keys) {
                    uniqueKeywords.insert(dbName);
                }
            }
        }
    } else if (std::holds_alternative<MySQLDatabaseNode*>(databaseNode)) {
        auto* mysqlNode = std::get<MySQLDatabaseNode*>(databaseNode);
        if (mysqlNode && mysqlNode->parentDb) {
            MySQLDatabase* mysqlDb = mysqlNode->parentDb;

            // Add table names and column names
            if (mysqlNode->tablesLoaded) {
                for (const auto& table : mysqlNode->tables) {
                    uniqueKeywords.insert(table.name);
                    for (const auto& column : table.columns) {
                        uniqueKeywords.insert(column.name);
                    }
                }
            }

            // Add view names and column names
            if (mysqlNode->viewsLoaded) {
                for (const auto& view : mysqlNode->views) {
                    uniqueKeywords.insert(view.name);
                    for (const auto& column : view.columns) {
                        uniqueKeywords.insert(column.name);
                    }
                }
            }

            // Add database names if in multi-database mode
            if (mysqlDb->getConnectionInfo().showAllDatabases) {
                const auto& databaseDataMap = mysqlDb->getDatabaseDataMap();
                for (const auto& dbName : databaseDataMap | std::views::keys) {
                    uniqueKeywords.insert(dbName);
                }
            }
        }
    } else if (std::holds_alternative<SQLiteDatabase*>(databaseNode)) {
        auto* sqliteNode = std::get<SQLiteDatabase*>(databaseNode);
        if (sqliteNode) {
            // Add table names and column names
            if (sqliteNode->areTablesLoaded()) {
                for (const auto& table : sqliteNode->getTables()) {
                    uniqueKeywords.insert(table.name);
                    for (const auto& column : table.columns) {
                        uniqueKeywords.insert(column.name);
                    }
                }
            }

            // Add view names and column names
            for (const auto& view : sqliteNode->getViews()) {
                uniqueKeywords.insert(view.name);
                for (const auto& column : view.columns) {
                    uniqueKeywords.insert(column.name);
                }
            }
        }
    }

    const std::vector extraKeywords(uniqueKeywords.begin(), uniqueKeywords.end());
    sqlEditor.SetExtraKeywords(extraKeywords);
}

bool SQLEditorTab::renderVerticalSplitter(const char* id, float* position, float minSize1,
                                          float minSize2) const {
    constexpr float hoverThickness = 6.0f;
    constexpr float visualThickness = 2.0f;

    // invisible button with larger hover area
    ImGui::InvisibleButton(id, ImVec2(-1, hoverThickness));

    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();

    if (hovered || held) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    bool changed = false;
    if (held) {
        const float delta = ImGui::GetIO().MouseDelta.y;
        if (delta != 0.0f) {
            // Use the stored total height for consistent calculation
            const float availableHeight = totalContentHeight;

            // Calculate current pixel position
            const float currentPixelPos = *position * availableHeight;
            const float newPixelPos = currentPixelPos + delta;

            // Convert back to normalized position
            float newPosition = newPixelPos / availableHeight;

            // Apply constraints
            const float minPos1 = minSize1 / availableHeight;
            const float maxPos1 = 1.0f - (minSize2 / availableHeight);

            newPosition = std::max(minPos1, std::min(maxPos1, newPosition));

            if (newPosition != *position) {
                *position = newPosition;
                changed = true;
            }
        }
    }

    // Draw thin visual splitter line centered in the hover area
    const ImVec2 minPos = ImGui::GetItemRectMin();
    const ImVec2 maxPos = ImGui::GetItemRectMax();

    constexpr float centerOffset = (hoverThickness - visualThickness) * 0.5f;
    const auto visualMin = ImVec2(minPos.x, minPos.y + centerOffset);
    const auto visualMax = ImVec2(maxPos.x, minPos.y + centerOffset + visualThickness);

    const ImU32 col = ImGui::GetColorU32(held      ? ImGuiCol_SeparatorActive
                                         : hovered ? ImGuiCol_SeparatorHovered
                                                   : ImGuiCol_Separator);
    ImGui::GetWindowDrawList()->AddRectFilled(visualMin, visualMax, col);

    return changed;
}
