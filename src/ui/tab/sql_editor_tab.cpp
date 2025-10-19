#include "ui/tab/sql_editor_tab.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <set>
#include <variant>

// Constructor for PostgreSQL database node
SQLEditorTab::SQLEditorTab(const std::string& name, PostgresDatabaseNode* dbNode,
                           const std::shared_ptr<DatabaseInterface>& serverDatabase)
    : Tab(name, TabType::SQL_EDITOR), databaseNode(dbNode), serverDatabase(serverDatabase) {
    sqlEditor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);

    // Populate auto-complete with table and column names
    populateAutoCompleteKeywords();

    // Start loading schemas if not already loaded
    if (dbNode && !dbNode->schemasLoaded && !dbNode->loadingSchemas) {
        dbNode->startSchemasLoadAsync();
    }
}

// Constructor for MySQL database node
SQLEditorTab::SQLEditorTab(const std::string& name, MySQLDatabaseNode* dbNode,
                           const std::shared_ptr<DatabaseInterface>& serverDatabase)
    : Tab(name, TabType::SQL_EDITOR), databaseNode(dbNode), serverDatabase(serverDatabase) {
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
    if (serverDatabase) {
        auto pg = std::get<PostgresDatabaseNode*>(databaseNode);
        if (pg) {
            if (!selectedSchemaName.empty() &&
                serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                ImGui::Text("Server: %s | Database: %s | Schema: %s",
                            serverDatabase->getName().c_str(), pg->name.c_str(), pg->name.c_str());
            } else {
                ImGui::Text("Server: %s | Database: %s", serverDatabase->getName().c_str(),
                            pg->name.c_str());
            }
        } else {
            ImGui::Text("Server: %s (No database selected)", serverDatabase->getName().c_str());
        }
    } else {
        ImGui::Text("SQL Editor (No server connection)");
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
    if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
        ImGui::Text("Database:");
    } else {
        ImGui::Text("Schema:");
    }
    ImGui::SameLine();

    std::string currentSelection;
    if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
        auto mysql = std::get<MySQLDatabaseNode*>(databaseNode);
        currentSelection = mysql->name.empty() ? "None" : mysql->name;
    } else if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
        auto pg = std::get<PostgresDatabaseNode*>(databaseNode);
        currentSelection =
            selectedSchemaName.empty() ? "None" : (pg->name + "." + selectedSchemaName);
    }
    std::vector<DatabaseNode> availableDatabases;
    bool isLoadingAnySchemas = false;
    bool needsSchemasForTargetDb = false;

    if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
        auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
        const auto& databaseDataMap = pgDb->getDatabaseDataMap();
        for (const auto& [dbName, db] : databaseDataMap) {
            availableDatabases.push_back(db.get());
        }

        auto pg = std::get<PostgresDatabaseNode*>(databaseNode);
        std::string targetDb = pg->name.empty() ? pgDb->getDatabaseName() : pg->name;
        if (!targetDb.empty()) {
            const auto* dbData = pgDb->getDatabaseData(targetDb);
            if (dbData && dbData->loadingSchemas) {
                isLoadingAnySchemas = true;
            }

            if (dbData && selectedSchemaName.empty() && !dbData->schemasLoaded &&
                !dbData->loadingSchemas) {
                needsSchemasForTargetDb = true;
            }
        }

        if (pgDb->isLoadingSchemas() && targetDb == pgDb->getDatabaseName()) {
            isLoadingAnySchemas = true;
        }

        if (pgDb->isLoadingDatabases()) {
            isLoadingAnySchemas = true;
        }
    } else if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
        auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
        const auto& databaseDataMap = mysqlDb->getDatabaseDataMap();
        for (const auto& [dbName, db] : databaseDataMap) {
            availableDatabases.push_back(db.get());
        }

        if (mysqlDb->isLoadingDatabases()) {
            isLoadingAnySchemas = true;
        }
    }

    ImGui::SetNextItemWidth(200.0f);

    if (isLoadingAnySchemas || needsSchemasForTargetDb) {
        ImGui::BeginDisabled();
        std::string loadingText =
            (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL)
                ? "Loading databases..."
                : "Loading schemas...";
        if (ImGui::BeginCombo("##schema_combo", loadingText.c_str())) {
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        UIUtils::Spinner("##schema_loading_spinner", 8.0f, 2, ImGui::GetColorU32(ImGuiCol_Text));
    } else {
        if (ImGui::BeginCombo("##schema_combo", currentSelection.c_str())) {
            if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
                auto pg = std::get<PostgresDatabaseNode*>(databaseNode);
                if (pgDb->shouldShowAllDatabases()) {
                    const auto& databaseDataMap = pgDb->getDatabaseDataMap();
                    for (const auto& [dbName, dbDataPtr] : databaseDataMap) {
                        if (dbDataPtr) {
                            dbDataPtr->checkSchemasStatusAsync();
                        }
                    }

                    std::string targetDb = pg->name;
                    if (targetDb.empty()) {
                        targetDb = pgDb->getDatabaseName();
                    }
                    if (std::holds_alternative<std::monostate>(databaseNode)) {
                    }

                    if (!targetDb.empty()) {
                        auto* targetDbData = pgDb->getDatabaseData(targetDb);
                        if (targetDbData && !targetDbData->schemasLoaded &&
                            !targetDbData->loadingSchemas) {
                            if (targetDb == pgDb->getDatabaseName()) {
                                if (!pgDb->isLoadingSchemas()) {
                                    pgDb->refreshSchemas();
                                }
                            } else {
                                targetDbData->startSchemasLoadAsync();
                            }
                        }
                    }

                    for (const auto& [dbName, dbDataPtr] : databaseDataMap) {
                        if (dbName != targetDb && dbDataPtr) {
                            if (!dbDataPtr->schemasLoaded && !dbDataPtr->loadingSchemas) {
                                dbDataPtr->startSchemasLoadAsync();
                            }
                        }
                    }
                } else {
                    // Single database mode, schemas handled via refreshSchemas()
                }
            }

            if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
                if (ImGui::Selectable("None",
                                      std::holds_alternative<std::monostate>(databaseNode))) {
                    databaseNode.emplace<std::monostate>();
                }
            } else {
                if (ImGui::Selectable("None", selectedSchemaName.empty())) {
                    databaseNode.emplace<std::monostate>();
                    selectedSchemaName.clear();
                }
            }

            if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
                auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
                auto pg = std::get<MySQLDatabaseNode*>(databaseNode);
                if (mysqlDb) {
                    if (mysqlDb->isLoadingDatabases()) {
                        mysqlDb->checkDatabasesStatusAsync();
                    }

                    for (const auto& dbName : availableDatabases) {
                        auto db = std::get<MySQLDatabaseNode*>(dbName);
                        const bool isSelected = (pg->name == db->name);
                        if (ImGui::Selectable(db->name.c_str(), isSelected)) {
                            databaseNode = dbName;
                            selectedSchemaName.clear();
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }

                    if (mysqlDb->isLoadingDatabases()) {
                        ImGui::Text("  Loading databases...");
                    }
                }
            } else if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
                auto pgNode = std::get<PostgresDatabaseNode*>(databaseNode);
                if (pgDb) {
                    for (const auto& dbName : availableDatabases) {
                        auto db = std::get<PostgresDatabaseNode*>(dbName);
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
                        } else if (!db->loadingSchemas) {
                            db->startSchemasLoadAsync();
                        }

                        for (const auto& schemaName : schemas) {
                            ImGui::Indent(16.0f);
                            const bool isSelected =
                                (pgNode->name == db->name && selectedSchemaName == schemaName);
                            const std::string schemaLabel = "  " + schemaName;
                            const std::string schemaId =
                                std::format("{}##{}_{}", schemaLabel, db->name, schemaName);

                            if (ImGui::Selectable(schemaId.c_str(), isSelected)) {
                                databaseNode = dbName;
                                selectedSchemaName = schemaName;
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                            ImGui::Unindent(16.0f);
                        }

                        if (db->name == pgNode->name && db->loadingSchemas) {
                            ImGui::Indent(16.0f);
                            ImGui::Text("  Loading schemas...");
                            ImGui::Unindent(16.0f);
                        } else if (db->name != pgNode->name) {
                            if (db->loadingSchemas) {
                                ImGui::Indent(16.0f);
                                ImGui::Text("  Loading schemas...");
                                ImGui::Unindent(16.0f);
                            } else if (!db->schemasLoaded && schemas.empty()) {
                                ImGui::Indent(16.0f);
                                ImGui::Text("  Click to load schemas...");
                                ImGui::Unindent(16.0f);
                            }
                        }
                    }
                }
            }

            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
}

void SQLEditorTab::renderQueryResults() {
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
        try {
            if (shouldCancelQuery) {
                return;
            }
            if (serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                auto db = std::get<PostgresDatabaseNode*>(databaseNode);
            }

            // Check for cancellation before executing query
            if (shouldCancelQuery) {
                return;
            }

            // Time the query execution
            const auto startTime = std::chrono::high_resolution_clock::now();

            // For Redis, also get the text result to check for errors
            std::string textResult;
            // if (targetDb->getType() == DatabaseType::REDIS) {
            //     textResult = targetDb->executeQuery(query);
            // }

            // // Get structured results for table display
            // auto [columnNames, tableData] = targetDb->executeQueryStructured(query);

            // const auto endTime = std::chrono::high_resolution_clock::now();
            // lastQueryDuration =
            //     std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            // // Check for cancellation before setting results
            // if (shouldCancelQuery) {
            //     return;
            // }

            // // For Redis, check if the text result contains an error
            // if (targetDb->getType() == DatabaseType::REDIS && textResult.find("Error:") == 0) {
            //     queryResult = textResult;
            //     queryError = textResult;
            //     hasStructuredResults = false;
            //     queryColumnNames.clear();
            //     queryTableData.clear();
            //     return;
            // }

            // queryColumnNames = columnNames;
            // queryTableData = tableData;
            // hasStructuredResults = true;

            // // Clear any previous error
            // queryError.clear();
            // queryResult.clear();
        } catch (const std::exception& e) {
            queryResult = "Error executing query: " + std::string(e.what());
            queryError = queryResult;
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
    if (!serverDatabase || !serverDatabase->isConnected()) {
        return;
    }

    std::set<std::string> uniqueKeywords;

    // Add table names and collect column names
    for (const auto& table : serverDatabase->getTables()) {
        uniqueKeywords.insert(table.name);

        // Collect unique column names
        for (const auto& column : table.columns) {
            uniqueKeywords.insert(column.name);
        }
    }

    // Add view names and collect column names
    for (const auto& view : serverDatabase->getViews()) {
        uniqueKeywords.insert(view.name);

        // Collect unique column names from views
        for (const auto& column : view.columns) {
            uniqueKeywords.insert(column.name);
        }
    }

    // For PostgreSQL, add schema names
    if (serverDatabase->getType() == DatabaseType::POSTGRESQL) {
        const auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
        if (pgDb) {
            // Add schema names
            for (const auto& schemaPtr : pgDb->getSchemas()) {
                if (schemaPtr) {
                    uniqueKeywords.insert(schemaPtr->name);
                }
            }

            // Add database names if in multi-database mode
            if (pgDb->shouldShowAllDatabases()) {
                const auto& databaseDataMap = pgDb->getDatabaseDataMap();
                for (const auto& [dbName, _] : databaseDataMap) {
                    uniqueKeywords.insert(dbName);
                }
            }
        }
    }
    // For MySQL, add database names
    else if (serverDatabase->getType() == DatabaseType::MYSQL) {
        const auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
        if (mysqlDb && mysqlDb->shouldShowAllDatabases()) {
            for (const auto& dbName : mysqlDb->getDatabaseNames()) {
                uniqueKeywords.insert(dbName);
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
