#include "ui/tab/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql/mysql_database_node.hpp"
#include "database/postgres/postgres_database_node.hpp"
#include "database/postgres/postgres_schema_node.hpp"
#include "imgui.h"
#include <algorithm>
#include <format>
#include <iostream>
#include <ranges>
#include <set>
#include <utility>

// Constructor for PostgreSQL schema
DiagramTab::DiagramTab(const std::string& name, PostgresSchemaNode* schemaNode)
    : Tab(name, TabType::DIAGRAM), databaseNode(schemaNode) {
    initializeEditor();
    loadDatabaseSchema();
}

// Constructor for MySQL database
DiagramTab::DiagramTab(const std::string& name, MySQLDatabaseNode* dbNode)
    : Tab(name, TabType::DIAGRAM), databaseNode(dbNode) {
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
    config.SettingsFile = nullptr;
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
    if (!editorContext) {
        ImGui::Text("Error: Node editor context not initialized");
        return;
    }

    if (!schemaLoaded) {
        ImGui::Text("Loading database schema...");

        if (!isLoadingSchema) {
            isLoadingSchema = true;
            loadDatabaseSchema();
        }

        // Check async loading status
        if (auto* schemaNode = std::get_if<PostgresSchemaNode*>(&databaseNode)) {
            if (*schemaNode) {
                (*schemaNode)->checkTablesStatusAsync();
                if ((*schemaNode)->tablesLoaded) {
                    isLoadingSchema = false;
                }
            }
        } else if (auto* mysqlNode = std::get_if<MySQLDatabaseNode*>(&databaseNode)) {
            if (*mysqlNode) {
                (*mysqlNode)->checkTablesStatusAsync();
                if ((*mysqlNode)->tablesLoaded) {
                    isLoadingSchema = false;
                }
            }
        }
        return;
    }

    // Toolbar
    if (auto schemaNode = std::get<PostgresSchemaNode*>(databaseNode)) {
        if (schemaNode && schemaNode->parentDbNode && schemaNode->parentDbNode->parentDb) {
            auto toolBarName =
                std::format("Schema: {}.{}", schemaNode->parentDbNode->name, schemaNode->name);
            ImGui::Text("Schema: %s", toolBarName.c_str());
        } else {
            ImGui::Text("Schema: (disconnected)");
        }
    } else if (auto* mysqlNode = std::get_if<MySQLDatabaseNode*>(&databaseNode)) {
        if (*mysqlNode && (*mysqlNode)->parentDb) {
            ImGui::Text("Database: %s", (*mysqlNode)->name.c_str());
        } else {
            ImGui::Text("Database: (disconnected)");
        }
    } else {
        ImGui::Text("Database: (no database selected)");
    }

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
    const std::string editorId =
        "Database Diagram##" + std::to_string(reinterpret_cast<uintptr_t>(this));
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

std::vector<Table> DiagramTab::getTablesForDiagram() const {
    std::vector<Table> tables;

    if (auto* schemaNode = std::get_if<PostgresSchemaNode*>(&databaseNode)) {
        if (*schemaNode) {
            return (*schemaNode)->tables;
        }
    } else if (auto* mysqlNode = std::get_if<MySQLDatabaseNode*>(&databaseNode)) {
        if (*mysqlNode) {
            return (*mysqlNode)->tables;
        }
    }

    return tables;
}

void DiagramTab::loadDatabaseSchema() {
    nodes.clear();
    links.clear();
    tableToNodeIdMap.clear();
    foreignKeyCache.clear(); // Clear the foreign key cache
    // Start with higher IDs to avoid conflicts with internal node editor IDs
    nextNodeId = 1000;
    nextLinkId = 10000;
    nextPinId = 100000;

    std::vector<Table> tables;

    if (auto* schemaNode = std::get_if<PostgresSchemaNode*>(&databaseNode)) {
        if (*schemaNode) {
            // Check if tables are loaded
            if (!(*schemaNode)->tablesLoaded && !(*schemaNode)->tablesLoader.isRunning()) {
                (*schemaNode)->startTablesLoadAsync();
            }

            // If tables are still loading, wait
            if ((*schemaNode)->tablesLoader.isRunning()) {
                schemaLoaded = false; // Keep trying
                return;
            }

            tables = (*schemaNode)->tables;
        }
    } else if (auto* mysqlNode = std::get_if<MySQLDatabaseNode*>(&databaseNode)) {
        if (*mysqlNode) {
            // Check if tables are loaded
            if (!(*mysqlNode)->tablesLoaded && !(*mysqlNode)->loadingTables) {
                (*mysqlNode)->startTablesLoadAsync();
            }

            // If tables are still loading, wait
            if ((*mysqlNode)->loadingTables) {
                schemaLoaded = false; // Keep trying
                return;
            }

            tables = (*mysqlNode)->tables;
        }
    }

    if (tables.empty()) {
        schemaLoaded = true;
        return;
    }

    // Create nodes for each table with better spacing
    ImVec2 position(100, 100);
    constexpr float horizontalSpacing = 400.0f;
    constexpr float verticalSpacing = 350.0f;
    constexpr float maxWidth = 1600.0f;

    for (const auto& table : tables) {
        createTableNode(table, position);
        position.x += horizontalSpacing;
        if (position.x > maxWidth) { // Wrap to next row
            position.x = 100;
            position.y += verticalSpacing;
        }
    }

    detectForeignKeys();

    schemaLoaded = true;
}

void DiagramTab::createTableNode(const Table& table, const ImVec2& position) {
    // Check if node already exists to prevent duplicates
    if (tableToNodeIdMap.contains(table.name)) {
        return;
    }

    DiagramNode node;
    node.id = ax::NodeEditor::NodeId(nextNodeId++);
    node.tableName = table.name;
    node.columns = table.columns;
    node.position = position;

    // Check if this is a primary table (has primary key)
    node.isPrimaryTable =
        std::ranges::any_of(table.columns, [](const Column& col) { return col.isPrimaryKey; });

    // Pre-allocate unique pin IDs for each column (including empty ones)
    node.columnPinIds.clear();
    node.columnPinIds.resize(table.columns.size()); // Use resize to ensure exact size
    for (size_t i = 0; i < table.columns.size(); ++i) {
        // Always allocate a pin ID even for empty columns to maintain index consistency
        node.columnPinIds[i] = ax::NodeEditor::PinId(nextPinId++);
    }

    nodes.push_back(node);
    tableToNodeIdMap[table.name] = node.id;
}

void DiagramTab::renderNodes() {
    if (nodes.empty()) {
        return;
    }

    // Get theme colors from application
    const auto& colors = Application::getInstance().getCurrentColors();

    // Use theme colors for better consistency
    const ImVec4 primaryTableColor = colors.yellow;   // Primary keys in yellow
    const ImVec4 normalTableColor = colors.text;      // Normal text color
    const ImVec4 normalColumnColor = colors.subtext1; // Slightly muted text
    const ImVec4 foreignKeyColor = colors.blue;       // Foreign keys in blue
    const ImVec4 typeColor = colors.subtext0;         // Type info in muted color
    const ImVec4 notNullColor = colors.red;           // NOT NULL constraints in red

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

    for (auto& [id, tableName, columns, position, isPrimaryTable, initialPositionSet,
                columnPinIds] : nodes) {
        // Set initial position only once, then let the editor handle dragging
        if (!initialPositionSet) {
            ax::NodeEditor::SetNodePosition(id, position);
            initialPositionSet = true;
        }

        ax::NodeEditor::BeginNode(id);

        // Node header with table name
        ImGui::PushStyleColor(ImGuiCol_Text, isPrimaryTable ? primaryTableColor : normalTableColor);
        ImGui::Text(ICON_FA_TABLE " %s", tableName.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Columns - use indexed loop to access cached pin IDs
        for (size_t i = 0; i < columns.size(); ++i) {
            const auto& column = columns[i];

            // Skip empty column names
            if (column.name.empty()) {
                continue;
            }

            // Check if this column is a foreign key
            const bool isForeignKey = foreignKeyColumns.contains({tableName, column.name});

            // Don't push ImGui IDs - let node editor manage IDs
            ImGui::BeginGroup();

            // Create pin with cached ID - all pins are Input for simplicity
            ax::NodeEditor::BeginPin(columnPinIds[i], ax::NodeEditor::PinKind::Input);
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
                    std::string cacheKey = tableName + "." + column.name;
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
        position = ax::NodeEditor::GetNodePosition(id);
    }
}

void DiagramTab::renderLinks() {
    if (!showForeignKeys) {
        return;
    }

    if (links.empty()) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();

    const ImVec4 foreignKeyLinkColor = colors.sky;
    static constexpr float linkThickness = 2.5f;

    for (const auto& link : links) {
        ax::NodeEditor::Link(link.id, link.startPinId, link.endPinId, foreignKeyLinkColor,
                             linkThickness);
    }

    // Check for hovered link to show tooltip
    ax::NodeEditor::LinkId hoveredLinkId = ax::NodeEditor::GetHoveredLink();
    if (hoveredLinkId) {
        // Find the link details
        const auto linkIt = std::ranges::find_if(
            links, [hoveredLinkId](const DiagramLink& link) { return link.id == hoveredLinkId; });

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
        const auto nodeIt = std::ranges::find_if(
            nodes, [hoveredNodeId](const DiagramNode& node) { return node.id == hoveredNodeId; });

        if (nodeIt != nodes.end()) {
            // Show tooltip with table info
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Table: %s\nColumns: %zu", nodeIt->tableName.c_str(),
                                  nodeIt->columns.size());
            }

            // Handle double-click to open table viewer
            if (ImGui::IsMouseDoubleClicked(0)) {
                // TODO: Update to use PostgresSchemaNode for PostgreSQL databases
                // const auto& app = Application::getInstance();
                // app.getTabManager()->createTableViewerTab(database, nodeIt->tableName);
            }
        }
    }

    // Handle context menu
    ax::NodeEditor::NodeId contextNodeId;
    if (ax::NodeEditor::ShowNodeContextMenu(&contextNodeId)) {
        const auto nodeIt = std::ranges::find_if(
            nodes, [contextNodeId](const DiagramNode& node) { return node.id == contextNodeId; });

        if (nodeIt != nodes.end()) {
            ImGui::Text("Table: %s", nodeIt->tableName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("View Data")) {
                // TODO: Update to use PostgresSchemaNode for PostgreSQL databases
                // const auto& app = Application::getInstance();
                // app.getTabManager()->createTableViewerTab(database, nodeIt->tableName);
            }
            if (ImGui::MenuItem("New SQL Editor")) {
                const auto& app = Application::getInstance();
                // app.getTabManager()->createSQLEditorTab("", database);
            }
        }
    }
}

void DiagramTab::detectForeignKeys() {
    // Clear previous foreign key cache
    foreignKeyCache.clear();

    // Get foreign keys from table metadata
    const std::vector<Table> tables = getTablesForDiagram();

    for (const auto& table : tables) {
        // Use the foreign keys stored in the table structure
        for (const auto& fk : table.foreignKeys) {
            std::string cacheKey = table.name + "." + fk.sourceColumn;

            // Cache the foreign key relationship
            foreignKeyCache[cacheKey] = {fk.targetTable, fk.targetColumn};

            // Find the source node and column pin
            auto sourceNodeIt = tableToNodeIdMap.find(table.name);
            if (sourceNodeIt == tableToNodeIdMap.end())
                continue;

            // Find the target node
            auto targetNodeIt = tableToNodeIdMap.find(fk.targetTable);
            if (targetNodeIt == tableToNodeIdMap.end())
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
            if (foreignKeyCache.contains(cacheKey))
                continue;

            std::string referencedTable, referencedColumn;
            if (isForeignKeyColumn(node.tableName, column.name, referencedTable,
                                   referencedColumn)) {
                foreignKeyCache[cacheKey] = {referencedTable, referencedColumn};

                auto refTableIt = tableToNodeIdMap.find(referencedTable);
                if (refTableIt != tableToNodeIdMap.end()) {
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
        const std::string potentialTable = columnName.substr(0, columnName.length() - 3);

        // Look for plural form of table
        if (tableToNodeIdMap.contains(potentialTable + "s")) {
            referencedTable = potentialTable + "s";
            referencedColumn = "id";
            return true;
        }
        // Look for exact match
        if (tableToNodeIdMap.contains(potentialTable)) {
            referencedTable = potentialTable;
            referencedColumn = "id";
            return true;
        }
    }

    // Pattern 2: column name matches another table name + "_id"
    for (const auto& table : tableToNodeIdMap | std::views::keys) {
        if (columnName == table + "_id") {
            referencedTable = table;
            referencedColumn = "id";
            return true;
        }
    }

    // More sophisticated detection would query the database for actual FK constraints
    return false;
}
