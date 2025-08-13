#include "ui/tab.hpp"
#include "application.hpp"
#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"

#include "themes.hpp"
#include "ui/log_panel.hpp"
#include "ui/table_renderer.hpp"
#include "utils/spinner.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <iostream>
#include <utility>

// Base Tab class
Tab::Tab(std::string name, const TabType type) : name(std::move(name)), type(type) {}

// SQLEditorTab implementation
SQLEditorTab::SQLEditorTab(const std::string &name,
                           std::shared_ptr<DatabaseInterface> serverDatabase,
                           const std::string &selectedDatabaseName)
    : Tab(name, TabType::SQL_EDITOR), serverDatabase(serverDatabase),
      selectedDatabaseName(selectedDatabaseName), selectedSchemaName("") {
    sqlEditor.SetLanguageDefinition(TextEditor::LanguageDefinitionId::Sql);
    sqlEditor.SetShowWhitespacesEnabled(false);
    sqlEditor.SetShowLineNumbersEnabled(true);

    // Start loading schemas immediately for PostgreSQL databases
    if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
        auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
        if (pgDb) {
            if (pgDb->shouldShowAllDatabases()) {
                // Start loading database list if not already loaded (needed for combo)
                if (!pgDb->isLoadingDatabases() && pgDb->getDatabaseNames().empty()) {
                    pgDb->refreshDatabaseNames();
                }

                // Prioritize loading schemas for the target database first
                std::string targetDb = selectedDatabaseName;
                if (targetDb.empty()) {
                    targetDb = pgDb->getDatabaseName();
                }

                if (!targetDb.empty()) {
                    const auto &dbData = pgDb->getDatabaseData(targetDb);
                    if (!dbData.schemasLoaded && !dbData.loadingSchemas) {
                        // Load schemas for target database with high priority
                        if (targetDb != pgDb->getDatabaseName()) {
                            LogPanel::debug(
                                "Starting priority schema loading for target database: " +
                                targetDb);
                            pgDb->startSchemasLoadAsync(targetDb);
                        } else if (!pgDb->isLoadingSchemas()) {
                            LogPanel::debug("Starting schema loading for current database: " +
                                            targetDb);
                            pgDb->refreshSchemas();
                        }
                    }
                }
            } else {
                // Single database mode: load schemas for current database
                if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                    pgDb->refreshSchemas();
                }
            }
        }
    }
    // Start loading databases immediately for MySQL databases
    else if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
        auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
        if (mysqlDb) {
            if (mysqlDb->shouldShowAllDatabases()) {
                // Start loading database list if not already loaded (needed for combo)
                if (!mysqlDb->isLoadingDatabases() && mysqlDb->getDatabaseNames().empty()) {
                    mysqlDb->refreshDatabaseNames();
                }
            }
        }
    }
}

SQLEditorTab::~SQLEditorTab() {
    // Wait for any ongoing query execution to complete
    if (isExecutingQuery && queryExecutionFuture.valid()) {
        queryExecutionFuture.wait();
    }
}

void SQLEditorTab::render() {
    auto &app = Application::getInstance();

    // Check async query execution status
    checkQueryExecutionStatus();

    // Show database connection info if available
    if (serverDatabase) {
        if (!selectedDatabaseName.empty()) {
            if (!selectedSchemaName.empty() &&
                serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                ImGui::Text("Server: %s | Database: %s | Schema: %s",
                            serverDatabase->getName().c_str(), selectedDatabaseName.c_str(),
                            selectedSchemaName.c_str());
            } else {
                ImGui::Text("Server: %s | Database: %s", serverDatabase->getName().c_str(),
                            selectedDatabaseName.c_str());
            }
        } else {
            ImGui::Text("Server: %s (No database selected)", serverDatabase->getName().c_str());
        }
    } else {
        ImGui::Text("SQL Editor (No server connection)");
    }
    ImGui::Separator();

    // Calculate heights for splitter layout - store total height for splitter reference
    totalContentHeight = ImGui::GetContentRegionAvail().y;
    const float editorHeight = totalContentHeight * splitterPosition;
    const float resultsHeight =
        totalContentHeight * (1.0f - splitterPosition) - 6.0f; // 6px hover area for splitter

    // SQL Editor section
    if (ImGui::BeginChild("SQLEditor", ImVec2(-1, editorHeight), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        sqlEditor.Render("##SQL", true, ImVec2(-1, -1), true);
        sqlQuery = sqlEditor.GetText();
    }
    ImGui::EndChild();

    // Render splitter
    renderVerticalSplitter("##sql_splitter", &splitterPosition, 100.0f, 200.0f);

    // Results section
    if (ImGui::BeginChild("SQLResults", ImVec2(-1, resultsHeight), true,
                          ImGuiWindowFlags_NoScrollbar)) {
        // Buttons row
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
                std::shared_ptr<DatabaseInterface> targetDb = nullptr;

                if (serverDatabase) {
                    targetDb = serverDatabase;

                    // For Postgres and MySQL, ensure we're connected to the right database
                    if (!selectedDatabaseName.empty()) {
                        if (serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
                            if (pgDb && pgDb->getDatabaseName() != selectedDatabaseName) {
                                // Need to switch database first
                                auto [success, error] =
                                    pgDb->switchToDatabase(selectedDatabaseName);
                                if (!success) {
                                    queryResult = "Error switching to database '" +
                                                  selectedDatabaseName + "': " + error;
                                    queryError = queryResult;
                                    hasStructuredResults = false;
                                    queryColumnNames.clear();
                                    queryTableData.clear();
                                    targetDb = nullptr;
                                }
                            }
                        } else if (serverDatabase->getType() == DatabaseType::MYSQL) {
                            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
                            if (mysqlDb && mysqlDb->getDatabaseName() != selectedDatabaseName) {
                                // Need to switch database first
                                auto [success, error] =
                                    mysqlDb->switchToDatabase(selectedDatabaseName);
                                if (!success) {
                                    queryResult = "Error switching to database '" +
                                                  selectedDatabaseName + "': " + error;
                                    queryError = queryResult;
                                    hasStructuredResults = false;
                                    queryColumnNames.clear();
                                    queryTableData.clear();
                                    targetDb = nullptr;
                                }
                            }
                        }
                    }
                } else {
                    // Fall back to selected database if no server database set
                    const int selectedDb = app.getSelectedDatabase();
                    const auto &databases = app.getDatabases();
                    if (selectedDb >= 0 && selectedDb < static_cast<int>(databases.size())) {
                        targetDb = databases[selectedDb];
                        // Update the server database reference
                        serverDatabase = targetDb;
                    }
                }

                if (targetDb) {
                    startQueryExecutionAsync(targetDb, sqlQuery);
                } else {
                    queryResult =
                        "Error: No database selected. Please select a server and database.";
                    queryError = queryResult;
                    hasStructuredResults = false;
                    queryColumnNames.clear();
                    queryTableData.clear();
                }
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

        // Show schema helper when schema is selected
        if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL &&
            !selectedSchemaName.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Add SET search_path")) {
                std::string searchPathCmd =
                    "SET search_path TO \"" + selectedSchemaName + "\", public;\n";
                std::string currentText = sqlEditor.GetText();
                if (!currentText.empty() && !currentText.ends_with('\n')) {
                    searchPathCmd += "\n";
                }
                sqlEditor.SetText(searchPathCmd + currentText);
                sqlQuery = sqlEditor.GetText();
            }

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Add SET search_path command to use the selected schema");
            }
        }

        // Database and Schema selection dropdown
        ImGui::SameLine();
        if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
            ImGui::Text("Database:");
        } else {
            ImGui::Text("Schema:");
        }
        ImGui::SameLine();

        std::string currentSelection;
        if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
            currentSelection = selectedDatabaseName.empty() ? "None" : selectedDatabaseName;
        } else {
            currentSelection = selectedSchemaName.empty()
                                   ? "None"
                                   : (selectedDatabaseName + "." + selectedSchemaName);
        }
        std::vector<std::string> availableDatabases;
        bool isLoadingAnySchemas = false;
        bool needsSchemasForTargetDb = false;

        // Get available databases from the server
        if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
            if (pgDb && pgDb->shouldShowAllDatabases()) {
                availableDatabases = pgDb->getDatabaseNames();

                // Only check if target database schemas are loading (not all databases)
                std::string targetDb =
                    selectedDatabaseName.empty() ? pgDb->getDatabaseName() : selectedDatabaseName;
                if (!targetDb.empty()) {
                    const auto &dbData = pgDb->getDatabaseData(targetDb);
                    if (dbData.loadingSchemas) {
                        isLoadingAnySchemas = true;
                    }

                    // Check if we need schemas for the target database
                    if (selectedSchemaName.empty() && !dbData.schemasLoaded &&
                        !dbData.loadingSchemas) {
                        needsSchemasForTargetDb = true;
                    }
                }

                // Also check if we're actively loading the current database schemas
                if (pgDb->isLoadingSchemas() && targetDb == pgDb->getDatabaseName()) {
                    isLoadingAnySchemas = true;
                }

                // Also check if databases themselves are still loading
                if (pgDb->isLoadingDatabases()) {
                    isLoadingAnySchemas = true;
                }
            } else if (pgDb) {
                // Single database mode - just show the connected database
                availableDatabases.push_back(pgDb->getDatabaseName());
                isLoadingAnySchemas = pgDb->isLoadingSchemas();

                // Check if we need schemas for single database mode
                if (selectedSchemaName.empty() && !pgDb->areSchemasLoaded() &&
                    !pgDb->isLoadingSchemas()) {
                    needsSchemasForTargetDb = true;
                }
            }
        } else if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
            if (mysqlDb && mysqlDb->shouldShowAllDatabases()) {
                availableDatabases = mysqlDb->getDatabaseNames();

                // Check if databases are still loading
                if (mysqlDb->isLoadingDatabases()) {
                    isLoadingAnySchemas = true;
                }
            } else if (mysqlDb) {
                // Single database mode - just show the connected database
                availableDatabases.push_back(mysqlDb->getDatabaseName());
            }
        }

        ImGui::SetNextItemWidth(200.0f);

        // Show loading spinner and disable combo if still loading or need to start loading
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
            UIUtils::Spinner("##schema_loading_spinner", 8.0f, 2,
                             ImGui::GetColorU32(ImGuiCol_Text));
        } else {
            if (ImGui::BeginCombo("##schema_combo", currentSelection.c_str())) {
                // When combo is opened, prioritize target database and load others on-demand
                if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                    auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
                    if (pgDb) {
                        // Check async operations status
                        if (pgDb->isSwitchingDatabase()) {
                            pgDb->checkDatabaseSwitchStatusAsync();
                        }

                        // Check schema loading status for all databases
                        if (pgDb->shouldShowAllDatabases()) {
                            for (const auto &dbName : availableDatabases) {
                                pgDb->checkSchemasStatusAsync(dbName);
                            }

                            // Prioritize target database first, then load others on-demand
                            std::string targetDb = selectedDatabaseName;
                            if (targetDb.empty()) {
                                targetDb = pgDb->getDatabaseName();
                            }

                            // Ensure target database schemas are loaded first
                            if (!targetDb.empty()) {
                                const auto &targetDbData = pgDb->getDatabaseData(targetDb);
                                if (!targetDbData.schemasLoaded && !targetDbData.loadingSchemas) {
                                    if (targetDb == pgDb->getDatabaseName()) {
                                        if (!pgDb->isLoadingSchemas()) {
                                            pgDb->refreshSchemas();
                                        }
                                    } else {
                                        pgDb->startSchemasLoadAsync(targetDb);
                                    }
                                }
                            }

                            // Load other databases' schemas on-demand (only when combo is opened)
                            for (const auto &dbName : availableDatabases) {
                                if (dbName != targetDb) {
                                    const auto &dbData = pgDb->getDatabaseData(dbName);
                                    if (!dbData.schemasLoaded && !dbData.loadingSchemas) {
                                        pgDb->startSchemasLoadAsync(dbName);
                                    }
                                }
                            }
                        } else {
                            // For single database mode, just check current database schema status
                            pgDb->checkSchemasStatusAsync();
                        }
                    }
                }
                // Option to clear selection
                if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
                    if (ImGui::Selectable("None", selectedDatabaseName.empty())) {
                        selectedDatabaseName.clear();
                    }
                } else {
                    if (ImGui::Selectable("None", selectedSchemaName.empty())) {
                        selectedDatabaseName.clear();
                        selectedSchemaName.clear();
                    }
                }

                // For MySQL - show flat database list
                if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
                    auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
                    if (mysqlDb) {
                        // Check async database loading status
                        if (mysqlDb->isLoadingDatabases()) {
                            mysqlDb->checkDatabasesStatusAsync();
                        }

                        for (const auto &dbName : availableDatabases) {
                            const bool isSelected = (selectedDatabaseName == dbName);
                            if (ImGui::Selectable(dbName.c_str(), isSelected)) {
                                // Switch database if needed
                                if (mysqlDb->getDatabaseName() != dbName) {
                                    mysqlDb->switchToDatabaseAsync(dbName);
                                }
                                selectedDatabaseName = dbName;
                                selectedSchemaName.clear(); // Clear schema for MySQL
                            }
                            if (isSelected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }

                        // Show loading indicator if databases are being loaded
                        if (mysqlDb->isLoadingDatabases()) {
                            ImGui::Text("  Loading databases...");
                        }
                    }
                }
                // For PostgreSQL only - show hierarchical database/schema structure
                else if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
                    auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
                    if (pgDb) {
                        for (const auto &dbName : availableDatabases) {
                            // Show database name as a non-selectable label
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
                            ImGui::Text("%s", dbName.c_str());
                            ImGui::PopStyleColor();

                            // Get schemas for this database
                            std::vector<std::string> schemas;
                            if (dbName == pgDb->getDatabaseName()) {
                                // Current database - get schemas if loaded
                                if (pgDb->areSchemasLoaded()) {
                                    for (const auto &schema : pgDb->getSchemas()) {
                                        schemas.push_back(schema.name);
                                    }
                                } else if (!pgDb->isLoadingSchemas()) {
                                    // Start loading schemas if not already loading
                                    pgDb->refreshSchemas();
                                }
                            } else {
                                // Other databases - get cached schemas or load them
                                const auto &dbData = pgDb->getDatabaseData(dbName);
                                if (dbData.schemasLoaded) {
                                    for (const auto &schema : dbData.schemas) {
                                        schemas.push_back(schema.name);
                                    }
                                } else if (!dbData.loadingSchemas) {
                                    // Start loading schemas for this database using parallel
                                    // loading
                                    pgDb->startSchemasLoadAsync(dbName);
                                }
                            }

                            // Show schemas indented under the database
                            for (const auto &schemaName : schemas) {
                                ImGui::Indent(16.0f);
                                const bool isSelected = (selectedDatabaseName == dbName &&
                                                         selectedSchemaName == schemaName);
                                const std::string schemaLabel = "  " + schemaName;
                                // Create unique ID for each schema by combining database and schema
                                // name
                                const std::string schemaId =
                                    schemaLabel + "##" + dbName + "_" + schemaName;

                                if (ImGui::Selectable(schemaId.c_str(), isSelected)) {
                                    // Switch database if needed
                                    if (pgDb->getDatabaseName() != dbName) {
                                        pgDb->switchToDatabaseAsync(dbName);
                                        // Clear schema when switching databases
                                        selectedSchemaName.clear();
                                    }
                                    selectedDatabaseName = dbName;
                                    selectedSchemaName = schemaName;
                                }
                                if (isSelected) {
                                    ImGui::SetItemDefaultFocus();
                                }
                                ImGui::Unindent(16.0f);
                            }

                            // Show loading indicator if schemas are being loaded
                            if (dbName == pgDb->getDatabaseName() && pgDb->isLoadingSchemas()) {
                                ImGui::Indent(16.0f);
                                ImGui::Text("  Loading schemas...");
                                ImGui::Unindent(16.0f);
                            } else if (dbName != pgDb->getDatabaseName()) {
                                const auto &dbData = pgDb->getDatabaseData(dbName);
                                if (dbData.loadingSchemas) {
                                    ImGui::Indent(16.0f);
                                    ImGui::Text("  Loading schemas...");
                                    ImGui::Unindent(16.0f);
                                } else if (!dbData.schemasLoaded && schemas.empty()) {
                                    ImGui::Indent(16.0f);
                                    ImGui::Text("  Click to load schemas...");
                                    ImGui::Unindent(16.0f);
                                }
                            }

                            ImGui::Separator();
                        }
                    }
                }
                ImGui::EndCombo();
            }
        }

        // Always check async operations status to update UI when loading completes
        if (serverDatabase && serverDatabase->getType() == DatabaseType::POSTGRESQL) {
            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(serverDatabase);
            if (pgDb) {
                // Check database switch status
                if (pgDb->isSwitchingDatabase()) {
                    pgDb->checkDatabaseSwitchStatusAsync();
                }

                // Start loading schemas if needed for target database only
                if (needsSchemasForTargetDb) {
                    if (pgDb->shouldShowAllDatabases()) {
                        std::string targetDb = selectedDatabaseName.empty()
                                                   ? pgDb->getDatabaseName()
                                                   : selectedDatabaseName;
                        if (!targetDb.empty()) {
                            const auto &dbData = pgDb->getDatabaseData(targetDb);
                            if (!dbData.schemasLoaded && !dbData.loadingSchemas) {
                                if (targetDb != pgDb->getDatabaseName()) {
                                    pgDb->startSchemasLoadAsync(targetDb);
                                } else if (!pgDb->isLoadingSchemas()) {
                                    pgDb->refreshSchemas();
                                }
                            }
                        }
                    } else {
                        // Single database mode
                        if (!pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas()) {
                            pgDb->refreshSchemas();
                        }
                    }
                }

                // Check async status for essential operations only
                if (pgDb->shouldShowAllDatabases()) {
                    // Check database loading status (needed for combo to show database list)
                    if (pgDb->isLoadingDatabases()) {
                        pgDb->checkDatabasesStatusAsync();
                    }

                    // Only check schema status for target database and current database
                    std::string targetDb = selectedDatabaseName.empty() ? pgDb->getDatabaseName()
                                                                        : selectedDatabaseName;
                    if (!targetDb.empty()) {
                        pgDb->checkSchemasStatusAsync(targetDb);
                    }
                    if (targetDb != pgDb->getDatabaseName()) {
                        pgDb->checkSchemasStatusAsync(pgDb->getDatabaseName());
                    }
                } else {
                    // For single database mode, just check current database schema status
                    pgDb->checkSchemasStatusAsync();
                }

                // Auto-select default schema when target database schemas finish loading
                if (selectedSchemaName.empty()) {
                    std::string targetDb = selectedDatabaseName; // From constructor
                    if (targetDb.empty()) {
                        targetDb = pgDb->getDatabaseName(); // Fallback to current database
                    }

                    if (pgDb->shouldShowAllDatabases()) {
                        // Multi-database mode: prioritize the target database
                        if (!targetDb.empty()) {
                            const auto &dbData = pgDb->getDatabaseData(targetDb);
                            // Only auto-select if target database schemas are loaded and not
                            // loading
                            if (dbData.schemasLoaded && !dbData.loadingSchemas &&
                                !dbData.schemas.empty()) {
                                selectedDatabaseName = targetDb;
                                // Select "public" schema if available, otherwise first schema
                                for (const auto &schema : dbData.schemas) {
                                    if (schema.name == "public") {
                                        selectedSchemaName = schema.name;
                                        LogPanel::debug(
                                            "Auto-selected 'public' schema from target database: " +
                                            targetDb);
                                        break;
                                    }
                                }
                                if (selectedSchemaName.empty() && !dbData.schemas.empty()) {
                                    selectedSchemaName = dbData.schemas[0].name;
                                    LogPanel::debug("Auto-selected first schema '" +
                                                    selectedSchemaName +
                                                    "' from target database: " + targetDb);
                                }
                            }
                        }
                    } else {
                        // Single database mode: select current database
                        if (pgDb->areSchemasLoaded() && !pgDb->isLoadingSchemas() &&
                            !pgDb->getSchemas().empty()) {
                            selectedDatabaseName = pgDb->getDatabaseName();
                            // Select "public" schema if available, otherwise first schema
                            for (const auto &schema : pgDb->getSchemas()) {
                                if (schema.name == "public") {
                                    selectedSchemaName = schema.name;
                                    LogPanel::debug(
                                        "Auto-selected 'public' schema from current database: " +
                                        pgDb->getDatabaseName());
                                    break;
                                }
                            }
                            if (selectedSchemaName.empty() && !pgDb->getSchemas().empty()) {
                                selectedSchemaName = pgDb->getSchemas()[0].name;
                                LogPanel::debug(
                                    "Auto-selected first schema '" + selectedSchemaName +
                                    "' from current database: " + pgDb->getDatabaseName());
                            }
                        }
                    }
                }
            }
        } else if (serverDatabase && serverDatabase->getType() == DatabaseType::MYSQL) {
            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(serverDatabase);
            if (mysqlDb) {
                // Check database switch status
                if (mysqlDb->isSwitchingDatabase()) {
                    mysqlDb->checkDatabaseSwitchStatusAsync();
                }

                // Check database loading status if needed
                if (mysqlDb->shouldShowAllDatabases()) {
                    if (mysqlDb->isLoadingDatabases()) {
                        mysqlDb->checkDatabasesStatusAsync();
                    }
                }

                // Auto-select current database if no database is selected
                if (selectedDatabaseName.empty()) {
                    selectedDatabaseName = mysqlDb->getDatabaseName();
                }
            }
        }

        ImGui::Separator();

        // Results display
        if (!queryError.empty()) {
            // Show error message
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", queryError.c_str());
        } else if (hasStructuredResults && !queryColumnNames.empty()) {
            // Show results in table format using TableRenderer
            if (queryTableData.empty()) {
                // Show column headers even if no data
                ImGui::Text("No rows returned.");
                if (lastQueryDuration.count() > 0) {
                    ImGui::SameLine();
                    ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
                }
            } else {
                // Show row count and execution time on the same line at the top
                ImGui::Text("Rows: %zu", queryTableData.size());
                if (queryTableData.size() >= 1000) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "(limited to 1000 rows)");
                }
                if (lastQueryDuration.count() > 0) {
                    ImGui::SameLine();
                    ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
                }

                // Calculate available height for the table within the results child window
                float tableAvailableHeight =
                    ImGui::GetContentRegionAvail().y -
                    20.0f; // Reserve 20px for padding since row count is now above
                tableAvailableHeight =
                    std::max(tableAvailableHeight, 50.0f); // Ensure minimum height of 50px

                // Configure table renderer for query results
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
            // Query executed successfully but no results (e.g., INSERT, UPDATE, DELETE)
            ImGui::Text("Query executed successfully.");
            if (lastQueryDuration.count() > 0) {
                ImGui::SameLine();
                ImGui::Text("| Execution time: %lld ms", lastQueryDuration.count());
            }
        } else {
            // Fallback to text display or show empty state
            ImGui::Text("No results to display. Execute a query to see results here.");
        }
    }
    ImGui::EndChild(); // End SQLResults child window
}

void SQLEditorTab::startQueryExecutionAsync(const std::shared_ptr<DatabaseInterface> &targetDb,
                                            const std::string &query) {
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
    queryExecutionFuture = std::async(std::launch::async, [this, targetDb, query]() {
        try {
            if (shouldCancelQuery) {
                return;
            }

            // Connect to database if not already connected
            if (!targetDb->isConnected()) {
                auto [success, error] = targetDb->connect();
                if (!success) {
                    if (!shouldCancelQuery) {
                        queryResult = "Connection failed: " + error;
                        queryError = queryResult;
                    }
                    return;
                }
            }

            // Check for cancellation before executing query
            if (shouldCancelQuery) {
                return;
            }

            // Time the query execution
            const auto startTime = std::chrono::high_resolution_clock::now();

            // For Redis, also get the text result to check for errors
            std::string textResult;
            if (targetDb->getType() == DatabaseType::REDIS) {
                textResult = targetDb->executeQuery(query);
            }

            // Get structured results for table display
            auto [columnNames, tableData] = targetDb->executeQueryStructured(query);

            const auto endTime = std::chrono::high_resolution_clock::now();
            lastQueryDuration =
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

            // Check for cancellation before setting results
            if (shouldCancelQuery) {
                return;
            }

            // For Redis, check if the text result contains an error
            if (targetDb->getType() == DatabaseType::REDIS && textResult.find("Error:") == 0) {
                queryResult = textResult;
                queryError = textResult;
                hasStructuredResults = false;
                queryColumnNames.clear();
                queryTableData.clear();
                return;
            }

            queryColumnNames = columnNames;
            queryTableData = tableData;
            hasStructuredResults = true;

            // Clear any previous error
            queryError.clear();
            queryResult.clear();
        } catch (const std::exception &e) {
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
        } catch (const std::exception &e) {
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

bool SQLEditorTab::renderVerticalSplitter(const char *id, float *position, float minSize1,
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

// TableViewerTab implementation
TableViewerTab::TableViewerTab(const std::string &name, std::string databasePath,
                               std::string tableName,
                               std::shared_ptr<DatabaseInterface> serverDatabase)
    : Tab(name, TabType::TABLE_VIEWER), databasePath(std::move(databasePath)),
      tableName(std::move(tableName)), serverDatabase(std::move(serverDatabase)) {

    // Initialize table renderer with editable configuration
    TableRenderer::Config config;
    config.allowEditing = true;
    config.allowSelection = true;
    config.showRowNumbers = false;
    config.minHeight = 200.0f;

    tableRenderer = std::make_unique<TableRenderer>(config);

    // Set up callbacks
    tableRenderer->setOnCellEdit([this](int row, int col, const std::string &newValue) {
        if (newValue != tableData[row][col]) {
            tableData[row][col] = newValue;
            hasChanges = true;

            // Mark cell as edited
            if (row < editedCells.size() && col < editedCells[row].size()) {
                editedCells[row][col] = true;
            }
        }
    });

    tableRenderer->setOnCellSelect([this](int row, int col) { selectCell(row, col); });

    loadDataAsync();
}

void TableViewerTab::render() {
    const auto &colors =
        Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;

    checkAsyncLoadStatus();

    ImGui::Text("Table: %s", tableName.c_str());
    ImGui::Separator();

    // Pagination controls
    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;

    if (ImGui::Button("<<") && currentPage > 0) {
        firstPage();
    }
    ImGui::SameLine();

    if (ImGui::Button("<") && currentPage > 0) {
        previousPage();
    }
    ImGui::SameLine();

    ImGui::Text("Page %d of %d (%d rows total)", currentPage + 1, totalPages, totalRows);
    ImGui::SameLine();

    if (ImGui::Button(">") && currentPage < totalPages - 1) {
        nextPage();
    }
    ImGui::SameLine();

    if (ImGui::Button(">>") && currentPage < totalPages - 1) {
        lastPage();
    }

    // Action buttons next to pagination
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(20, 0)); // Add some spacing
    ImGui::SameLine();

    if (ImGui::Button("Refresh")) {
        refreshData();
    }

    // Show loading indicator
    if (isLoadingData) {
        ImGui::SameLine();
        ImGui::Text("Loading...");
    }
    ImGui::SameLine();

    if (hasChanges) {
        if (ImGui::Button("Save")) {
            saveChanges();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Save");
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (hasChanges) {
        if (ImGui::Button("Cancel")) {
            cancelChanges();
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Cancel");
        ImGui::EndDisabled();
    }

    if (hasChanges) {
        ImGui::SameLine();
        ImGui::TextColored(colors.peach, "Unsaved changes");
    }

    ImGui::Separator();

    // Show loading error if any
    if (hasLoadingError) {
        ImGui::PushStyleColor(ImGuiCol_Text, colors.red);
        ImGui::TextWrapped("Error loading data: %s", loadingError.c_str());
        ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            const std::string errorText = "Error loading data: " + loadingError;
            ImGui::SetClipboardText(errorText.c_str());
        }

        ImGui::Separator();
    }

    // Table display
    if (isLoadingData) {
        ImGui::Text("Loading table data...");
    } else if (!columnNames.empty() && !tableData.empty()) {
        // Update table renderer with current data
        tableRenderer->setColumns(columnNames);
        tableRenderer->setData(tableData);
        tableRenderer->setCellEditedStatus(editedCells);
        tableRenderer->setSelectedCell(selectedRow, selectedCol);

        tableRenderer->render("TableData");
    } else {
        ImGui::Text("No data to display");
    }

    // Check async SQL execution status
    checkSQLExecutionStatus();

    // Show save confirmation dialog if needed
    showSaveConfirmationDialog();
}

void TableViewerTab::loadData() {
    if (!serverDatabase || !serverDatabase->isConnected()) {
        return;
    }

    totalRows = serverDatabase->getRowCount(tableName);
    columnNames = serverDatabase->getColumnNames(tableName);

    // Get data with pagination
    const int offset = currentPage * rowsPerPage;
    tableData = serverDatabase->getTableData(tableName, rowsPerPage, offset);

    // Store original data for change tracking
    originalData = tableData;
    hasChanges = false;

    // Initialize edited cells tracking
    editedCells = std::vector<std::vector<bool>>(tableData.size(),
                                                 std::vector<bool>(columnNames.size(), false));
}

void TableViewerTab::nextPage() {
    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    if (currentPage < totalPages - 1 && !isLoadingData) {
        currentPage++;
        loadDataAsync();
    }
}

void TableViewerTab::previousPage() {
    if (currentPage > 0 && !isLoadingData) {
        currentPage--;
        loadDataAsync();
    }
}

void TableViewerTab::firstPage() {
    if (!isLoadingData) {
        currentPage = 0;
        loadDataAsync();
    }
}

void TableViewerTab::lastPage() {
    if (isLoadingData)
        return;

    const int totalPages = (totalRows + rowsPerPage - 1) / rowsPerPage;
    currentPage = totalPages - 1;
    loadDataAsync();
}

void TableViewerTab::refreshData() {
    if (!isLoadingData) {
        // Reset selection state
        selectedRow = -1;
        selectedCol = -1;
        hasChanges = false;
        hasLoadingError = false;
        loadingError.clear();

        // Clear edited cells tracking
        for (auto &row : editedCells) {
            std::fill(row.begin(), row.end(), false);
        }

        loadDataAsync();
    }
}

void TableViewerTab::saveChanges() {
    if (!hasChanges) {
        return;
    }

    // Generate SQL statements for the changes
    pendingUpdateSQL = generateUpdateSQL();

    if (pendingUpdateSQL.empty()) {
        // No valid SQL generated, just clear changes
        hasChanges = false;
        for (auto &row : editedCells) {
            std::fill(row.begin(), row.end(), false);
        }
        return;
    }

    // Show confirmation dialog
    showSaveDialog = true;
}

void TableViewerTab::cancelChanges() {
    // Restore original data
    tableData = originalData;
    hasChanges = false;

    // Clear edited cells tracking
    for (auto &row : editedCells) {
        std::fill(row.begin(), row.end(), false);
    }

    // Reset selection state
    selectedRow = -1;
    selectedCol = -1;
}

void TableViewerTab::selectCell(const int row, const int col) {
    const int tableSize = static_cast<int>(tableData.size());
    const int totalCols = static_cast<int>(columnNames.size());
    if (row >= 0 && row < tableSize && col >= 0 && col < totalCols) {
        selectedRow = row;
        selectedCol = col;
    }
}

void TableViewerTab::loadDataAsync() {
    if (!serverDatabase || !serverDatabase->isConnected()) {
        hasLoadingError = true;
        loadingError = "Database not found or not connected";
        return;
    }

    // Clear any previous async result
    if (serverDatabase->getType() == DatabaseType::SQLITE) {
        serverDatabase->clearTableDataResult();
    } else {
        serverDatabase->clearTableDataResult(tableName);
    }

    // Start async data loading (includes metadata)
    const int offset = currentPage * rowsPerPage;
    isLoadingData = true;
    hasLoadingError = false;
    loadingError.clear();

    serverDatabase->startTableDataLoadAsync(tableName, rowsPerPage, offset);
}

void TableViewerTab::checkAsyncLoadStatus() {
    if (!isLoadingData) {
        return;
    }

    if (!serverDatabase || !serverDatabase->isConnected()) {
        isLoadingData = false;
        hasLoadingError = true;
        loadingError = "Database not found or not connected";
        return;
    }

    // Always check the async status first
    if (serverDatabase->getType() == DatabaseType::SQLITE) {
        serverDatabase->checkTableDataStatusAsync();

        if (serverDatabase->hasTableDataResult()) {
            // Load completed - get all data including metadata
            tableData = serverDatabase->getTableDataResult();
            columnNames = serverDatabase->getColumnNamesResult();
            totalRows = serverDatabase->getRowCountResult();
            originalData = tableData;
            hasChanges = false;
            isLoadingData = false;

            // Initialize edited cells tracking
            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(columnNames.size(), false));

            // Clear the result to free memory
            serverDatabase->clearTableDataResult();
        } else if (!serverDatabase->isLoadingTableData()) {
            // Loading stopped but no result - probably an error
            isLoadingData = false;
            hasLoadingError = true;
            loadingError = "Failed to load table data";
        }
    } else {
        serverDatabase->checkTableDataStatusAsync(tableName);

        if (serverDatabase->hasTableDataResult(tableName)) {
            // Load completed - get all data including metadata
            tableData = serverDatabase->getTableDataResult(tableName);
            columnNames = serverDatabase->getColumnNamesResult(tableName);
            totalRows = serverDatabase->getRowCountResult(tableName);
            originalData = tableData;
            hasChanges = false;
            isLoadingData = false;

            // Initialize edited cells tracking
            editedCells = std::vector<std::vector<bool>>(
                tableData.size(), std::vector<bool>(columnNames.size(), false));

            // Clear the result to free memory
            serverDatabase->clearTableDataResult(tableName);
        } else if (!serverDatabase->isLoadingTableData(tableName)) {
            // Loading stopped but no result - probably an error
            isLoadingData = false;
            hasLoadingError = true;
            loadingError = "Failed to load table data";
        }
    }
}

std::vector<std::string> TableViewerTab::getPrimaryKeyColumns() const {
    std::vector<std::string> pkColumns;
    if (!serverDatabase || !serverDatabase->isConnected()) {
        return pkColumns;
    }

    // Find table columns
    const auto &tables = serverDatabase->getTables();
    for (const auto &table : tables) {
        if (table.name == tableName) {
            for (const auto &column : table.columns) {
                if (column.isPrimaryKey) {
                    pkColumns.push_back(column.name);
                }
            }
            break;
        }
    }

    return pkColumns;
}

std::vector<std::string> TableViewerTab::generateUpdateSQL() {
    std::vector<std::string> sqlStatements;

    if (!serverDatabase || !serverDatabase->isConnected()) {
        return sqlStatements;
    }

    const bool isSQLite = (serverDatabase->getType() == DatabaseType::SQLITE);
    const std::vector<std::string> pkColumns = getPrimaryKeyColumns();

    std::cout << "Generating UPDATE SQL for table: " << tableName << std::endl;
    std::cout << "Database type: " << (isSQLite ? "SQLite" : "Postgres") << std::endl;
    std::cout << "Primary key columns: ";
    for (const auto &pk : pkColumns) {
        std::cout << pk << " ";
    }
    std::cout << std::endl;

    // Process each edited cell
    for (int rowIdx = 0; rowIdx < editedCells.size(); rowIdx++) {
        for (int colIdx = 0; colIdx < editedCells[rowIdx].size(); colIdx++) {
            if (!editedCells[rowIdx][colIdx]) {
                continue; // Cell not edited
            }

            const std::string &columnName = columnNames[colIdx];
            const std::string &newValue = tableData[rowIdx][colIdx];

            // Build UPDATE statement
            std::string sql;
            if (isSQLite) {
                sql = std::format("UPDATE {} SET {} = ", tableName, columnName);
            } else {
                sql = std::format(R"(UPDATE "{}" SET "{}" = )", tableName, columnName);
            }

            // Add quoted value
            if (newValue == "NULL") {
                sql += "NULL";
            } else {
                sql += "'" + newValue + "'"; // Simple escaping - could be improved
            }

            sql += " WHERE ";

            // Build WHERE clause
            std::vector<std::string> whereConditions;

            if (!pkColumns.empty()) {
                // Use primary key columns
                for (const auto &pkCol : pkColumns) {
                    auto pkColIt = std::ranges::find(columnNames, pkCol);
                    if (pkColIt != columnNames.end()) {
                        const int pkColIdx =
                            static_cast<int>(std::distance(columnNames.begin(), pkColIt));
                        const std::string &pkValue = originalData[rowIdx][pkColIdx];

                        if (isSQLite) {
                            whereConditions.push_back(std::format("{} = '{}'", pkCol, pkValue));
                        } else {
                            whereConditions.push_back(std::format("\"{}\" = '{}'", pkCol, pkValue));
                        }
                    }
                }
            } else if (isSQLite) {
                // For SQLite without primary key, use all columns as condition to identify the row
                for (int condColIdx = 0; condColIdx < columnNames.size(); condColIdx++) {
                    const std::string &condValue = originalData[rowIdx][condColIdx];
                    if (condValue == "NULL") {
                        whereConditions.push_back(
                            std::format("{} IS NULL", columnNames[condColIdx]));
                    } else {
                        whereConditions.push_back(
                            std::format("{} = '{}'", columnNames[condColIdx], condValue));
                    }
                }
            } else {
                // For Postgres without primary key, use all columns as condition
                for (int condColIdx = 0; condColIdx < columnNames.size(); condColIdx++) {
                    const std::string &condValue = originalData[rowIdx][condColIdx];
                    if (condValue == "NULL") {
                        whereConditions.push_back(
                            std::format("\"{}\" IS NULL", columnNames[condColIdx]));
                    } else {
                        whereConditions.push_back(
                            std::format("\"{}\" = '{}'", columnNames[condColIdx], condValue));
                    }
                }
            }

            // Join conditions with AND
            for (int i = 0; i < whereConditions.size(); i++) {
                sql += whereConditions[i];
                if (i < whereConditions.size() - 1) {
                    sql += " AND ";
                }
            }

            sql += ";";
            sqlStatements.push_back(sql);
        }
    }

    return sqlStatements;
}

void TableViewerTab::showSaveConfirmationDialog() {
    if (!showSaveDialog) {
        return;
    }

    // Only open popup once when dialog first shows
    if (!dialogOpened) {
        ImGui::SetNextWindowSize(ImVec2(800, 600));
        ImGui::OpenPopup("Confirm Save Changes");
        dialogOpened = true;
    }

    if (ImGui::BeginPopupModal("Confirm Save Changes", nullptr)) {
        ImGui::Text("The following SQL statements will be executed:");
        ImGui::Separator();

        // Show SQL statements in a scrollable area
        if (ImGui::BeginChild("SQLPreview", ImVec2(0, -50), true)) {
            for (int i = 0; i < pendingUpdateSQL.size(); i++) {
                ImGui::Text("%d.", i + 1);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", pendingUpdateSQL[i].c_str());
                if (i < pendingUpdateSQL.size() - 1) {
                    ImGui::Separator();
                }
            }
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Buttons
        if (executingSQL) {
            // Show spinner and disable buttons during execution
            ImGui::BeginDisabled();
            ImGui::Button("Execute", ImVec2(120, 0));
            ImGui::EndDisabled();

            ImGui::SameLine();
            const auto &colors =
                Application::getInstance().isDarkTheme() ? Theme::NATIVE_DARK : Theme::NATIVE_LIGHT;
            UIUtils::Spinner("##spinner", 8.0f, 2, ImGui::GetColorU32(colors.blue));

            ImGui::SameLine();
            ImGui::Text("Executing...");
        } else {
            if (ImGui::Button("Execute", ImVec2(120, 0))) {
                if (serverDatabase && serverDatabase->isConnected()) {
                    executingSQL = true;

                    // Copy SQL statements and database pointer for async execution
                    auto sqlStatements = pendingUpdateSQL;

                    sqlExecutionFuture = std::async(
                        std::launch::async,
                        [db = serverDatabase, sqlStatements]() -> std::pair<bool, std::string> {
                            bool allSuccess = true;
                            std::string errorMessage;

                            for (const auto &sql : sqlStatements) {
                                std::cout << "Executing SQL: " << sql << std::endl;
                                const std::string result = db->executeQuery(sql);
                                std::cout << "SQL Result: " << result << std::endl;

                                if (result.find("Error:") == 0) {
                                    allSuccess = false;
                                    errorMessage = result;
                                    std::cerr << "SQL execution failed: " << result << std::endl;
                                    break;
                                }
                            }

                            if (allSuccess) {
                                std::cout << "All SQL statements executed successfully"
                                          << std::endl;
                            }

                            return {allSuccess, errorMessage};
                        });
                } else {
                    std::cerr << "Database not found or not connected" << std::endl;
                }
            }
        }

        ImGui::SameLine();
        if (executingSQL) {
            ImGui::BeginDisabled();
            ImGui::Button("Cancel", ImVec2(120, 0));
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                showSaveDialog = false;
                pendingUpdateSQL.clear();
                dialogOpened = false;
            }
        }

        ImGui::EndPopup();
    } else if (!ImGui::IsPopupOpen("Confirm Save Changes")) {
        // Dialog was closed by clicking outside or ESC
        showSaveDialog = false;
        pendingUpdateSQL.clear();
        dialogOpened = false;
    }
}

void TableViewerTab::checkSQLExecutionStatus() {
    if (!executingSQL || !sqlExecutionFuture.valid()) {
        return;
    }

    // Check if async execution is complete
    if (sqlExecutionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto [success, errorMessage] = sqlExecutionFuture.get();

            if (success) {
                // Mark changes as saved
                hasChanges = false;
                originalData = tableData;
                for (auto &row : editedCells) {
                    std::fill(row.begin(), row.end(), false);
                }
            } else {
                std::cerr << "Failed to execute SQL statements: " << errorMessage << std::endl;
                // Keep the dialog open but reset execution state
            }

            // Reset execution state
            executingSQL = false;

            // Close dialog on successful execution, keep open on error for user to see
            if (success) {
                showSaveDialog = false;
                pendingUpdateSQL.clear();
                dialogOpened = false;
            }

        } catch (const std::exception &e) {
            std::cerr << "Exception during SQL execution: " << e.what() << std::endl;
            executingSQL = false;
        }
    }
}
