#include "ui/tab/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include <algorithm>
#include <iostream>
#include <set>

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

    handleZoomShortcuts();

    // Use a unique identifier for each diagram editor instance
    std::string editorId = "Database Diagram##" + std::to_string(reinterpret_cast<uintptr_t>(this));
    ax::NodeEditor::Begin(editorId.c_str(), ImVec2(0.0, 0.0f));

    renderNodes();
    renderLinks();
    handleNodeInteraction();

    ax::NodeEditor::End();
    ax::NodeEditor::SetCurrentEditor(nullptr);
}

void DiagramTab::handleZoomShortcuts() {
    auto& io = ImGui::GetIO();

    const bool shortcutDown = io.KeyCtrl || io.KeySuper;
    if (!shortcutDown) {
        return;
    }

    if (!ax::NodeEditor::AreShortcutsEnabled()) {
        return;
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        return;
    }

    float wheelAdjustment = 0.0f;

    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
        wheelAdjustment += 1.0f;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
        wheelAdjustment -= 1.0f;
    }

    if (wheelAdjustment != 0.0f) {
        io.MouseWheel += wheelAdjustment;
    }
}

std::vector<Table> DiagramTab::getTablesForDiagram() {
    std::vector<Table> tables;

    if (!database || !database->isConnected()) {
        return tables;
    }

    // For PostgreSQL and MySQL, we need to get tables for a specific database
    if (database->getType() == DatabaseType::POSTGRESQL) {
        auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
        if (pgDb) {
            std::string dbToUse =
                targetDatabaseName.empty() ? pgDb->getDatabaseName() : targetDatabaseName;
            auto& dbData = pgDb->getDatabaseData(dbToUse);
            tables = dbData.tables;
        }
    } else if (database->getType() == DatabaseType::MYSQL) {
        auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
        if (mysqlDb) {
            std::string dbToUse =
                targetDatabaseName.empty() ? mysqlDb->getDatabaseName() : targetDatabaseName;
            auto& dbData = mysqlDb->getDatabaseData(dbToUse);
            tables = dbData.tables;
        }
    } else {
        // For SQLite and other databases, use the standard interface
        tables = database->getTables();
    }

    return tables;
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
    static const ImVec4 foreignKeyColor(0.4f, 0.7f, 1.0f, 1.0f);
    static const ImVec4 typeColor(0.6f, 0.6f, 0.6f, 1.0f);
    static const ImVec4 notNullColor(0.8f, 0.4f, 0.4f, 1.0f);

    // Build a set of foreign key columns for quick lookup
    std::set<std::pair<std::string, std::string>> foreignKeyColumns;
    for (const auto& link : links) {
        foreignKeyColumns.insert({link.fromTable, link.fromColumn});
    }

    // Also build a set of referenced columns
    std::set<std::pair<std::string, std::string>> referencedColumns;
    for (const auto& link : links) {
        referencedColumns.insert({link.toTable, link.toColumn});
    }

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

            // Check if this column is a foreign key
            bool isForeignKey = foreignKeyColumns.count({node.tableName, column.name}) > 0;

            // Don't push ImGui IDs - let node editor manage IDs
            ImGui::BeginGroup();

            // Create pin with cached ID - all pins are Input for simplicity
            ax::NodeEditor::BeginPin(node.columnPinIds[i], ax::NodeEditor::PinKind::Input);
            if (isForeignKey && showForeignKeys) {
                ImGui::PushStyleColor(ImGuiCol_Text, foreignKeyColor);
                ImGui::Text("●");
                ImGui::PopStyleColor();
            } else {
                ImGui::Text("●");
            }
            ax::NodeEditor::EndPin();

            ImGui::SameLine();

            // Column name with icons
            if (showPrimaryKeys && column.isPrimaryKey) {
                ImGui::PushStyleColor(ImGuiCol_Text, primaryTableColor);
                ImGui::Text(ICON_FA_KEY " %s", column.name.c_str());
                ImGui::PopStyleColor();
            } else if (isForeignKey && showForeignKeys) {
                // Highlight foreign keys with special icon and color
                ImGui::PushStyleColor(ImGuiCol_Text, foreignKeyColor);
                ImGui::Text(ICON_FA_LINK " %s", column.name.c_str());
                ImGui::PopStyleColor();

                // Show tooltip with relationship info
                if (ImGui::IsItemHovered()) {
                    std::string cacheKey = node.tableName + "." + column.name;
                    auto fkIt = foreignKeyCache.find(cacheKey);
                    if (fkIt != foreignKeyCache.end()) {
                        ImGui::SetTooltip("Foreign Key → %s.%s", fkIt->second.first.c_str(),
                                          fkIt->second.second.c_str());
                    }
                }
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

    // Debug: Check if we have any links
    if (links.empty()) {
        return;
    }

    // Use a more vibrant color and thicker line for foreign key relationships
    static const ImVec4 foreignKeyLinkColor(0.3f, 0.6f, 1.0f, 1.0f); // Bright blue
    static const float linkThickness = 2.5f; // Slightly thinner for cleaner look

    for (const auto& link : links) {
        ax::NodeEditor::Link(link.id, link.startPinId, link.endPinId, foreignKeyLinkColor,
                             linkThickness);
    }

    // Check for hovered link to show tooltip
    ax::NodeEditor::LinkId hoveredLinkId = ax::NodeEditor::GetHoveredLink();
    if (hoveredLinkId) {
        // Find the link details
        auto linkIt =
            std::find_if(links.begin(), links.end(), [hoveredLinkId](const DiagramLink& link) {
                return link.id == hoveredLinkId;
            });

        if (linkIt != links.end() && ImGui::BeginTooltip()) {
            ImGui::Text("%s.%s", linkIt->fromTable.c_str(), linkIt->fromColumn.c_str());
            ImGui::Text("  ↓");
            ImGui::Text("%s.%s", linkIt->toTable.c_str(), linkIt->toColumn.c_str());
            ImGui::EndTooltip();
        }
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

    // Get foreign keys from table metadata
    std::vector<Table> tables = getTablesForDiagram();

    for (const auto& table : tables) {
        // Use the foreign keys stored in the table structure
        for (const auto& fk : table.foreignKeys) {
            std::string cacheKey = table.name + "." + fk.sourceColumn;

            // Cache the foreign key relationship
            foreignKeyCache[cacheKey] = {fk.targetTable, fk.targetColumn};

            // Find the source node and column pin
            auto sourceNodeIt = tableToNodeId.find(table.name);
            if (sourceNodeIt == tableToNodeId.end())
                continue;

            // Find the target node
            auto targetNodeIt = tableToNodeId.find(fk.targetTable);
            if (targetNodeIt == tableToNodeId.end())
                continue;

            // Find the source column pin ID
            ax::NodeEditor::PinId sourcePinId(0);
            for (const auto& node : nodes) {
                if (node.tableName == table.name) {
                    for (size_t colIdx = 0; colIdx < node.columns.size(); ++colIdx) {
                        if (node.columns[colIdx].name == fk.sourceColumn) {
                            sourcePinId = node.columnPinIds[colIdx];
                            break;
                        }
                    }
                    break;
                }
            }

            // Find the target column pin ID
            ax::NodeEditor::PinId targetPinId(0);
            for (const auto& node : nodes) {
                if (node.tableName == fk.targetTable) {
                    for (size_t colIdx = 0; colIdx < node.columns.size(); ++colIdx) {
                        if (node.columns[colIdx].name == fk.targetColumn) {
                            targetPinId = node.columnPinIds[colIdx];
                            break;
                        }
                    }
                    break;
                }
            }

            // Create link if both pins are found
            if (sourcePinId && targetPinId) {
                DiagramLink link;
                link.id = ax::NodeEditor::LinkId(nextLinkId++);
                link.startPinId = sourcePinId;
                link.endPinId = targetPinId;
                link.fromTable = table.name;
                link.toTable = fk.targetTable;
                link.fromColumn = fk.sourceColumn;
                link.toColumn = fk.targetColumn;

                links.push_back(link);
            }
        }
    }

    // Fall back to heuristic detection if no foreign keys are defined in metadata
    if (links.empty()) {
        detectForeignKeysHeuristic();
    }
}

void DiagramTab::detectForeignKeysHeuristic() {
    // Use naming conventions as a fallback when metadata is not available
    for (const auto& node : nodes) {
        for (size_t colIdx = 0; colIdx < node.columns.size(); ++colIdx) {
            const auto& column = node.columns[colIdx];

            std::string cacheKey = node.tableName + "." + column.name;
            if (foreignKeyCache.find(cacheKey) != foreignKeyCache.end())
                continue;

            std::string referencedTable, referencedColumn;
            if (isForeignKeyColumn(node.tableName, column.name, referencedTable,
                                   referencedColumn)) {
                foreignKeyCache[cacheKey] = {referencedTable, referencedColumn};

                auto refTableIt = tableToNodeId.find(referencedTable);
                if (refTableIt != tableToNodeId.end()) {
                    ax::NodeEditor::PinId endPinId(0);
                    for (const auto& targetNode : nodes) {
                        if (targetNode.tableName == referencedTable) {
                            for (size_t targetColIdx = 0; targetColIdx < targetNode.columns.size();
                                 ++targetColIdx) {
                                if (targetNode.columns[targetColIdx].name == referencedColumn) {
                                    endPinId = targetNode.columnPinIds[targetColIdx];
                                    break;
                                }
                            }
                            break;
                        }
                    }

                    if (endPinId) {
                        DiagramLink link;
                        link.id = ax::NodeEditor::LinkId(nextLinkId++);
                        link.startPinId = node.columnPinIds[colIdx];
                        link.endPinId = endPinId;
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
