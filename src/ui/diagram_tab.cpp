#include "ui/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
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
        // Try to load the schema again
        loadDatabaseSchema();

        // Check async loading status for PostgreSQL and MySQL
        if (database->getType() == DatabaseType::POSTGRESQL) {
            auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
            if (pgDb) {
                pgDb->checkTablesStatusAsync();
            }
        } else if (database->getType() == DatabaseType::MYSQL) {
            auto mysqlDb = std::dynamic_pointer_cast<MySQLDatabase>(database);
            if (mysqlDb) {
                mysqlDb->checkTablesStatusAsync();
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
    ImGui::SameLine();
    ImGui::Checkbox("Auto Layout", &autoLayout);
    if (autoLayout) {
        ImGui::SameLine();
        ImGui::SliderFloat("Node Spacing", &nodeSpacing, 200.0f, 500.0f);
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
    ax::NodeEditor::Begin("Database Diagram", ImVec2(0.0, 0.0f));

    renderNodes();
    renderLinks();
    handleNodeInteraction();

    ax::NodeEditor::End();
    ax::NodeEditor::SetCurrentEditor(nullptr);
}

void DiagramTab::loadDatabaseSchema() {
    if (!database || !database->isConnected()) {
        std::cout << "DiagramTab: Database not connected" << std::endl;
        return;
    }

    std::cout << "DiagramTab: Starting to load database schema for " << database->getName()
              << std::endl;

    nodes.clear();
    links.clear();
    tableToNodeId.clear();
    nextNodeId = 1;
    nextLinkId = 1;
    nextPinId = 1;

    // For PostgreSQL and MySQL, we need to get tables for a specific database
    std::vector<Table> tables;
    if (database->getType() == DatabaseType::POSTGRESQL) {
        auto pgDb = std::dynamic_pointer_cast<PostgresDatabase>(database);
        if (pgDb) {
            std::string dbToUse =
                targetDatabaseName.empty() ? pgDb->getDatabaseName() : targetDatabaseName;
            std::cout << "DiagramTab: Loading tables for PostgreSQL database: " << dbToUse
                      << std::endl;

            // For PostgreSQL, if the target database is different from current,
            // we need to switch to it first
            if (!targetDatabaseName.empty() && targetDatabaseName != pgDb->getDatabaseName()) {
                std::cout << "DiagramTab: Target database " << dbToUse
                          << " is different from current database " << pgDb->getDatabaseName()
                          << ", tables may not be available" << std::endl;
            }

            // Get the tables for the specific database
            auto& dbData = pgDb->getDatabaseData(dbToUse);
            if (!dbData.tablesLoaded && !dbData.loadingTables) {
                std::cout << "DiagramTab: Tables not loaded for " << dbToUse
                          << ", triggering load..." << std::endl;
                // For the target database, we need to trigger table loading
                // This is normally done when expanding the tree, but we'll do it here too
                if (dbToUse == pgDb->getDatabaseName()) {
                    // If it's the current database, use the regular refresh
                    pgDb->refreshTables();
                } else {
                    // For a different database, we need to switch to it first
                    std::cout << "DiagramTab: Need to switch to database " << dbToUse
                              << " to load tables" << std::endl;
                    schemaLoaded = true;
                    return;
                }
            }

            // If tables are still loading, wait
            if (dbData.loadingTables) {
                std::cout << "DiagramTab: Tables are loading for " << dbToUse << "..." << std::endl;
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
            std::cout << "DiagramTab: Loading tables for MySQL database: " << dbToUse << std::endl;

            // For MySQL, if the target database is different from current,
            // we need to switch to it first
            if (!targetDatabaseName.empty() && targetDatabaseName != mysqlDb->getDatabaseName()) {
                std::cout << "DiagramTab: Target database " << dbToUse
                          << " is different from current database " << mysqlDb->getDatabaseName()
                          << ", tables may not be available" << std::endl;
            }

            // Get the tables for the specific database
            auto& dbData = mysqlDb->getDatabaseData(dbToUse);
            if (!dbData.tablesLoaded && !dbData.loadingTables) {
                std::cout << "DiagramTab: Tables not loaded for " << dbToUse
                          << ", triggering load..." << std::endl;
                // For the target database, we need to trigger table loading
                if (dbToUse == mysqlDb->getDatabaseName()) {
                    // If it's the current database, use the regular refresh
                    mysqlDb->refreshTables();
                } else {
                    // For a different database, we need to switch to it first
                    std::cout << "DiagramTab: Need to switch to database " << dbToUse
                              << " to load tables" << std::endl;
                    schemaLoaded = true;
                    return;
                }
            }

            // If tables are still loading, wait
            if (dbData.loadingTables) {
                std::cout << "DiagramTab: Tables are loading for " << dbToUse << "..." << std::endl;
                schemaLoaded = false; // Keep trying
                return;
            }

            tables = dbData.tables;
        }
    } else {
        // For SQLite and other databases, use the standard interface
        if (!database->areTablesLoaded()) {
            std::cout << "DiagramTab: Tables not loaded, refreshing..." << std::endl;
            database->refreshTables();
        }
        tables = database->getTables();
    }

    std::cout << "DiagramTab: Found " << tables.size() << " tables" << std::endl;

    if (tables.empty()) {
        schemaLoaded = true;
        return;
    }

    // Create nodes for each table
    ImVec2 position(100, 100);
    for (const auto& table : tables) {
        std::cout << "DiagramTab: Creating node for table: " << table.name << std::endl;
        createTableNode(table, position);
        if (autoLayout) {
            position.x += nodeSpacing;
            if (position.x > 1200) { // Wrap to next row
                position.x = 100;
                position.y += 200;
            }
        }
    }

    std::cout << "DiagramTab: Created " << nodes.size() << " nodes" << std::endl;

    // Detect and create foreign key relationships
    std::cout << "DiagramTab: Detecting foreign keys..." << std::endl;
    detectForeignKeys();
    std::cout << "DiagramTab: Foreign key detection completed" << std::endl;

    if (autoLayout) {
        std::cout << "DiagramTab: Performing auto layout..." << std::endl;
        autoLayoutNodes();
        std::cout << "DiagramTab: Auto layout completed" << std::endl;
    }

    schemaLoaded = true;
    std::cout << "DiagramTab: Schema loaded successfully" << std::endl;
}

void DiagramTab::createTableNode(const Table& table, const ImVec2& position) {
    DiagramNode node;
    node.id = ax::NodeEditor::NodeId(nextNodeId++);
    node.tableName = table.name;
    node.columns = table.columns;
    node.position = position;

    // Check if this is a primary table (has primary key)
    node.isPrimaryTable = std::any_of(table.columns.begin(), table.columns.end(),
                                      [](const Column& col) { return col.isPrimaryKey; });

    nodes.push_back(node);
    tableToNodeId[table.name] = node.id;
}

void DiagramTab::renderNodes() {
    if (nodes.empty()) {
        return;
    }

    for (auto& node : nodes) {
        // Safety check for node position
        if (std::isnan(node.position.x) || std::isnan(node.position.y) ||
            std::isinf(node.position.x) || std::isinf(node.position.y)) {
            node.position = ImVec2(100, 100);
        }

        ax::NodeEditor::SetNodePosition(node.id, node.position);
        ax::NodeEditor::BeginNode(node.id);

        // Node header with table name
        ImGui::PushStyleColor(ImGuiCol_Text, node.isPrimaryTable ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f)
                                                                 : ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
        ImGui::Text(ICON_FA_TABLE " %s", node.tableName.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Columns
        for (const auto& column : node.columns) {
            // Safety check for column name
            if (column.name.empty()) {
                continue;
            }

            ImGui::BeginGroup();

            // Pin for potential connections
            ax::NodeEditor::BeginPin(ax::NodeEditor::PinId(nextPinId++),
                                     ax::NodeEditor::PinKind::Input);
            ImGui::Text("●");
            ax::NodeEditor::EndPin();

            ImGui::SameLine();

            // Column name with icons
            std::string columnText = column.name;
            if (showPrimaryKeys && column.isPrimaryKey) {
                columnText = ICON_FA_KEY " " + columnText;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
            }

            ImGui::Text("%s", columnText.c_str());
            ImGui::PopStyleColor();

            // Show column type if enabled
            if (showColumnTypes && !column.type.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::Text("(%s)", column.type.c_str());
                ImGui::PopStyleColor();
            }

            // Show NOT NULL constraint
            if (column.isNotNull && !column.isPrimaryKey) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.4f, 0.4f, 1.0f));
                ImGui::Text("NOT NULL");
                ImGui::PopStyleColor();
            }

            ImGui::EndGroup();
        }

        ax::NodeEditor::EndNode();

        // Store node position for layout
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
    // Simple foreign key detection based on naming conventions
    // This is a basic implementation - in a real scenario, you'd query the database
    // for actual foreign key constraints

    for (const auto& node : nodes) {
        for (const auto& column : node.columns) {
            std::string referencedTable, referencedColumn;
            if (isForeignKeyColumn(node.tableName, column.name, referencedTable,
                                   referencedColumn)) {
                // Find the referenced table node
                auto refTableIt = tableToNodeId.find(referencedTable);
                if (refTableIt != tableToNodeId.end()) {
                    DiagramLink link;
                    link.id = ax::NodeEditor::LinkId(nextLinkId++);
                    link.startPinId = ax::NodeEditor::PinId(nextPinId++);
                    link.endPinId = ax::NodeEditor::PinId(nextPinId++);
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

void DiagramTab::autoLayoutNodes() {
    if (nodes.empty()) {
        return;
    }

    // Simple grid layout
    const float spacing = nodeSpacing;
    const int maxColumns = 4;

    int currentColumn = 0;
    int currentRow = 0;

    for (auto& node : nodes) {
        ImVec2 newPosition;
        newPosition.x = 100 + currentColumn * spacing;
        newPosition.y = 100 + currentRow * 250;

        // Just update the position in our node structure
        // Don't call NodeEditor functions here as the context may not be set
        node.position = newPosition;

        currentColumn++;
        if (currentColumn >= maxColumns) {
            currentColumn = 0;
            currentRow++;
        }
    }
}
