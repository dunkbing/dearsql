#include "ui/tab/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include <algorithm>
#include <iostream>

DiagramTab::DiagramTab(const std::string& name, std::shared_ptr<DatabaseInterface> database,
                       const std::string& targetDatabaseName)
    : Tab(name, TabType::DIAGRAM), database(std::move(database)),
      targetDatabaseName(targetDatabaseName) {
    initializeEditor();
    loadDatabaseSchema();
}

DiagramTab::~DiagramTab() {
    if (editorContext) {
        ax::NodeEditor::DestroyEditor(editorContext);
    }
}

void DiagramTab::initializeEditor() {
    ax::NodeEditor::Config config;
    config.SettingsFile = nullptr; // Don't save editor state to file
    config.BeginSaveSession = nullptr;
    config.EndSaveSession = nullptr;
    config.SaveSettings = nullptr;
    config.LoadSettings = nullptr;
    config.SaveNodeSettings = nullptr;
    config.LoadNodeSettings = nullptr;
    config.UserPointer = nullptr;
    config.CustomZoomLevels = ImVector<float>();
    config.CanvasSizeMode = ax::NodeEditor::CanvasSizeMode::FitVerticalView;
    config.DragButtonIndex = 0;
    config.SelectButtonIndex = 0;
    config.NavigateButtonIndex = 1;
    config.ContextMenuButtonIndex = 1;
    config.EnableSmoothZoom = false;
    config.SmoothZoomPower = 1.1f;

    editorContext = ax::NodeEditor::CreateEditor(&config);

    if (!editorContext) {
        std::cerr << "Failed to create node editor context!" << std::endl;
    }
}

void DiagramTab::render() {
    if (!database || !database->isConnected()) {
        ImGui::Text("Database not connected");
        return;
    }

    if (!editorContext) {
        ImGui::Text("Error: Node editor context not initialized");
        return;
    }

    if (!schemaLoaded) {
        ImGui::Text("Loading database schema...");

        // Only try to load if not already loading
        if (!isLoadingSchema) {
            isLoadingSchema = true;
            loadDatabaseSchema();
        }

        // Check async loading status for PostgreSQL and MySQL
        if (database->getType() == DatabaseType::POSTGRESQL) {
            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
            if (pgDb) {
                pgDb->checkTablesStatusAsync();
                // Reset loading flag if tables are loaded
                auto& dbData = pgDb->getDatabaseData(
                    targetDatabaseName.empty() ? pgDb->getDatabaseName() : targetDatabaseName);
                if (dbData.tablesLoaded || (!dbData.loadingTables && !dbData.tables.empty())) {
                    isLoadingSchema = false;
                }
            }
        } else if (database->getType() == DatabaseType::MYSQL) {
            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
            if (mysqlDb) {
                mysqlDb->checkTablesStatusAsync();
                // Reset loading flag if tables are loaded
                auto& dbData = mysqlDb->getDatabaseData(
                    targetDatabaseName.empty() ? mysqlDb->getDatabaseName() : targetDatabaseName);
                if (dbData.tablesLoaded || (!dbData.loadingTables && !dbData.tables.empty())) {
                    isLoadingSchema = false;
                }
            }
        } else {
            // For other databases, reset flag if tables are loaded
            if (database->areTablesLoaded()) {
                isLoadingSchema = false;
            }
        }
        return;
    }

    // Toolbar
    ImGui::Text("Database: %s", database->getName().c_str());
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Refresh")) {
        schemaLoaded = false;
        loadDatabaseSchema();
    }
    ImGui::Separator();

    // Options
    ImGui::Checkbox("Show Column Types", &showColumnTypes);
    ImGui::SameLine();
    ImGui::Checkbox("Show Primary Keys", &showPrimaryKeys);
    ImGui::SameLine();
    ImGui::Checkbox("Show Foreign Keys", &showForeignKeys);

    ImGui::Separator();

    // Node editor
    if (!editorContext) {
        std::cout << "DiagramTab: Editor context is null, cannot render!" << std::endl;
        return;
    }

    ax::NodeEditor::SetCurrentEditor(editorContext);

    // Use a unique identifier for each diagram editor instance
    std::string editorId = "Database Diagram##" + std::to_string(reinterpret_cast<uintptr_t>(this));
    ax::NodeEditor::Begin(editorId.c_str(), ImVec2(0.0, 0.0f));

    renderNodes();
    renderLinks();
    handleNodeInteraction();

    ax::NodeEditor::End();
    ax::NodeEditor::SetCurrentEditor(nullptr);
}

void DiagramTab::loadDatabaseSchema() {
    if (!database || !database->isConnected()) {
        return;
    }

    nodes.clear();
    links.clear();
    tableToNodeId.clear();
    foreignKeyCache.clear(); // Clear the foreign key cache
    // Start with higher IDs to avoid conflicts with internal node editor IDs
    nextNodeId = 1000;
    nextLinkId = 10000;
    nextPinId = 100000;

    // For PostgreSQL and MySQL, we need to get tables for a specific database
    std::vector<Table> tables;
    if (database->getType() == DatabaseType::POSTGRESQL) {
        auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
        if (pgDb) {
            std::string dbToUse =
                targetDatabaseName.empty() ? pgDb->getDatabaseName() : targetDatabaseName;

            // For PostgreSQL, if the target database is different from current,
            // we need to switch to it first
            if (!targetDatabaseName.empty() && targetDatabaseName != pgDb->getDatabaseName()) {
                // Tables may not be available for different database
            }

            // Get the tables for the specific database
            auto& dbData = pgDb->getDatabaseData(dbToUse);
            if (!dbData.tablesLoaded && !dbData.loadingTables) {
                // For the target database, we need to trigger table loading
                // This is normally done when expanding the tree, but we'll do it here too
                if (dbToUse == pgDb->getDatabaseName()) {
                    // If it's the current database, use the regular refresh
                    pgDb->refreshTables();
                } else {
                    // For a different database, we need to switch to it first
                    schemaLoaded = true;
                    return;
                }
            }

            // If tables are still loading, wait
            if (dbData.loadingTables) {
                schemaLoaded = false; // Keep trying
                return;
            }

            tables = dbData.tables;
        }
    } else if (database->getType() == DatabaseType::MYSQL) {
        auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
        if (mysqlDb) {
            std::string dbToUse =
                targetDatabaseName.empty() ? mysqlDb->getDatabaseName() : targetDatabaseName;

            // For MySQL, if the target database is different from current,
            // we need to switch to it first
            if (!targetDatabaseName.empty() && targetDatabaseName != mysqlDb->getDatabaseName()) {
                // Tables may not be available for different database
            }

            // Get the tables for the specific database
            auto& dbData = mysqlDb->getDatabaseData(dbToUse);
            if (!dbData.tablesLoaded && !dbData.loadingTables) {
                // For the target database, we need to trigger table loading
                if (dbToUse == mysqlDb->getDatabaseName()) {
                    // If it's the current database, use the regular refresh
                    mysqlDb->refreshTables();
                } else {
                    // For a different database, we need to switch to it first
                    schemaLoaded = true;
                    return;
                }
            }

            // If tables are still loading, wait
            if (dbData.loadingTables) {
                schemaLoaded = false; // Keep trying
                return;
            }

            tables = dbData.tables;
        }
    } else {
        // For SQLite and other databases, use the standard interface
        if (!database->areTablesLoaded()) {
            database->refreshTables();
        }
        tables = database->getTables();
    }

    if (tables.empty()) {
        schemaLoaded = true;
        return;
    }

    // Create nodes for each table with better spacing
    ImVec2 position(100, 100);
    const float horizontalSpacing = 400.0f; // Increased from nodeSpacing (300)
    const float verticalSpacing = 350.0f;   // Increased from 200
    const float maxWidth = 1600.0f;         // Increased from 1200

    for (const auto& table : tables) {
        createTableNode(table, position);
        position.x += horizontalSpacing;
        if (position.x > maxWidth) { // Wrap to next row
            position.x = 100;
            position.y += verticalSpacing;
        }
    }

    // Detect and create foreign key relationships
    detectForeignKeys();

    schemaLoaded = true;
}

void DiagramTab::createTableNode(const Table& table, const ImVec2& position) {
    // Check if node already exists to prevent duplicates
    if (tableToNodeId.find(table.name) != tableToNodeId.end()) {
        return;
    }

    DiagramNode node;
    node.id = ax::NodeEditor::NodeId(nextNodeId++);
    node.tableName = table.name;
    node.columns = table.columns;
    node.position = position;

    // Check if this is a primary table (has primary key)
    node.isPrimaryTable = std::any_of(table.columns.begin(), table.columns.end(),
                                      [](const Column& col) { return col.isPrimaryKey; });

    // Pre-allocate unique pin IDs for each column (including empty ones)
    node.columnPinIds.clear();
    node.columnPinIds.resize(table.columns.size()); // Use resize to ensure exact size
    for (size_t i = 0; i < table.columns.size(); ++i) {
        // Always allocate a pin ID even for empty columns to maintain index consistency
        node.columnPinIds[i] = ax::NodeEditor::PinId(nextPinId++);
    }

    nodes.push_back(node);
    tableToNodeId[table.name] = node.id;
}

void DiagramTab::renderNodes() {
    if (nodes.empty()) {
        return;
    }

    // Cache style colors to avoid repeated calculations
    static const ImVec4 primaryTableColor(1.0f, 0.8f, 0.2f, 1.0f);
    static const ImVec4 normalTableColor(0.8f, 0.8f, 0.8f, 1.0f);
    static const ImVec4 normalColumnColor(0.9f, 0.9f, 0.9f, 1.0f);
    static const ImVec4 typeColor(0.6f, 0.6f, 0.6f, 1.0f);
    static const ImVec4 notNullColor(0.8f, 0.4f, 0.4f, 1.0f);

    for (size_t nodeIdx = 0; nodeIdx < nodes.size(); ++nodeIdx) {
        auto& node = nodes[nodeIdx];

        // Set initial position only once, then let the editor handle dragging
        if (!node.initialPositionSet) {
            ax::NodeEditor::SetNodePosition(node.id, node.position);
            node.initialPositionSet = true;
        }

        ax::NodeEditor::BeginNode(node.id);

        // Node header with table name
        ImGui::PushStyleColor(ImGuiCol_Text,
                              node.isPrimaryTable ? primaryTableColor : normalTableColor);
        ImGui::Text(ICON_FA_TABLE " %s", node.tableName.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Columns - use indexed loop to access cached pin IDs
        for (size_t i = 0; i < node.columns.size(); ++i) {
            const auto& column = node.columns[i];

            // Skip empty column names
            if (column.name.empty()) {
                continue;
            }

            // Don't push ImGui IDs - let node editor manage IDs
            ImGui::BeginGroup();

            // Create pin with cached ID
            ax::NodeEditor::BeginPin(node.columnPinIds[i], ax::NodeEditor::PinKind::Input);
            ImGui::Text("●");
            ax::NodeEditor::EndPin();

            ImGui::SameLine();

            // Column name with icons
            if (showPrimaryKeys && column.isPrimaryKey) {
                ImGui::PushStyleColor(ImGuiCol_Text, primaryTableColor);
                ImGui::Text(ICON_FA_KEY " %s", column.name.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, normalColumnColor);
                ImGui::Text("%s", column.name.c_str());
                ImGui::PopStyleColor();
            }

            // Show column type if enabled
            if (showColumnTypes && !column.type.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, typeColor);
                ImGui::Text("(%s)", column.type.c_str());
                ImGui::PopStyleColor();
            }

            // Show NOT NULL constraint
            if (column.isNotNull && !column.isPrimaryKey) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, notNullColor);
                ImGui::Text("NOT NULL");
                ImGui::PopStyleColor();
            }

            ImGui::EndGroup();
        }

        ax::NodeEditor::EndNode();

        // Always update position to track dragging
        node.position = ax::NodeEditor::GetNodePosition(node.id);
    }
}

void DiagramTab::renderLinks() {
    if (!showForeignKeys) {
        return;
    }

    for (const auto& link : links) {
        ax::NodeEditor::Link(link.id, link.startPinId, link.endPinId,
                             ImVec4(0.6f, 0.8f, 1.0f, 1.0f), 2.0f);
    }
}

void DiagramTab::handleNodeInteraction() {
    // Handle node creation (if needed in the future)
    // Handle link creation (if needed in the future)

    // For now, just handle selection and context menus
    ax::NodeEditor::NodeId hoveredNodeId = ax::NodeEditor::GetHoveredNode();
    if (hoveredNodeId) {
        // Find the table name for the hovered node
        auto nodeIt =
            std::find_if(nodes.begin(), nodes.end(), [hoveredNodeId](const DiagramNode& node) {
                return node.id == hoveredNodeId;
            });

        if (nodeIt != nodes.end()) {
            // Show tooltip with table info
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Table: %s\nColumns: %zu", nodeIt->tableName.c_str(),
                                  nodeIt->columns.size());
            }

            // Handle double-click to open table viewer
            if (ImGui::IsMouseDoubleClicked(0)) {
                auto& app = Application::getInstance();
                app.getTabManager()->createTableViewerTab(database, nodeIt->tableName);
            }
        }
    }

    // Handle context menu
    ax::NodeEditor::NodeId contextNodeId;
    if (ax::NodeEditor::ShowNodeContextMenu(&contextNodeId)) {
        auto nodeIt =
            std::find_if(nodes.begin(), nodes.end(), [contextNodeId](const DiagramNode& node) {
                return node.id == contextNodeId;
            });

        if (nodeIt != nodes.end()) {
            ImGui::Text("Table: %s", nodeIt->tableName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("View Data")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createTableViewerTab(database, nodeIt->tableName);
            }
            if (ImGui::MenuItem("New SQL Editor")) {
                auto& app = Application::getInstance();
                app.getTabManager()->createSQLEditorTab("", database);
            }
        }
    }
}

void DiagramTab::detectForeignKeys() {
    // Clear previous foreign key cache
    foreignKeyCache.clear();

    // Simple foreign key detection based on naming conventions
    // This is a basic implementation - in a real scenario, you'd query the database
    // for actual foreign key constraints

    for (const auto& node : nodes) {
        for (size_t colIdx = 0; colIdx < node.columns.size(); ++colIdx) {
            const auto& column = node.columns[colIdx];

            // Create cache key
            std::string cacheKey = node.tableName + "." + column.name;

            // Check if we've already processed this foreign key
            if (foreignKeyCache.find(cacheKey) == foreignKeyCache.end()) {
                std::string referencedTable, referencedColumn;
                if (isForeignKeyColumn(node.tableName, column.name, referencedTable,
                                       referencedColumn)) {
                    // Cache the result
                    foreignKeyCache[cacheKey] = {referencedTable, referencedColumn};

                    // Find the referenced table node
                    auto refTableIt = tableToNodeId.find(referencedTable);
                    if (refTableIt != tableToNodeId.end()) {
                        // Find the referenced node and column to get its pin ID
                        ax::NodeEditor::PinId endPinId(0);
                        for (const auto& targetNode : nodes) {
                            if (targetNode.tableName == referencedTable) {
                                // Find the referenced column index
                                for (size_t targetColIdx = 0;
                                     targetColIdx < targetNode.columns.size(); ++targetColIdx) {
                                    if (targetNode.columns[targetColIdx].name == referencedColumn) {
                                        endPinId = targetNode.columnPinIds[targetColIdx];
                                        break;
                                    }
                                }
                                break;
                            }
                        }

                        // Only create link if we found the target pin
                        if (endPinId) {
                            DiagramLink link;
                            link.id = ax::NodeEditor::LinkId(nextLinkId++);
                            link.startPinId = node.columnPinIds[colIdx]; // Use cached pin ID
                            link.endPinId = endPinId; // Use the target column's cached pin ID
                            link.fromTable = node.tableName;
                            link.toTable = referencedTable;
                            link.fromColumn = column.name;
                            link.toColumn = referencedColumn;

                            links.push_back(link);
                        }
                    }
                }
            }
        }
    }
}

bool DiagramTab::isForeignKeyColumn(const std::string& tableName, const std::string& columnName,
                                    std::string& referencedTable, std::string& referencedColumn) {
    // Basic foreign key detection based on common naming patterns

    // Pattern 1: column ends with "_id" and there's a table with that name
    const std::string suffix = "_id";
    if (columnName.length() > suffix.length() &&
        columnName.substr(columnName.length() - suffix.length()) == suffix) {
        std::string potentialTable = columnName.substr(0, columnName.length() - 3);

        // Look for plural form of table
        if (tableToNodeId.find(potentialTable + "s") != tableToNodeId.end()) {
            referencedTable = potentialTable + "s";
            referencedColumn = "id";
            return true;
        }
        // Look for exact match
        if (tableToNodeId.find(potentialTable) != tableToNodeId.end()) {
            referencedTable = potentialTable;
            referencedColumn = "id";
            return true;
        }
    }

    // Pattern 2: column name matches another table name + "_id"
    for (const auto& [table, nodeId] : tableToNodeId) {
        if (columnName == table + "_id") {
            referencedTable = table;
            referencedColumn = "id";
            return true;
        }
    }

    // More sophisticated detection would query the database for actual FK constraints
    return false;
}
