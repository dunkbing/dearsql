#include "ui/tab/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "imgui.h"
#include <algorithm>
#include <iostream>
#include <ranges>
#include <set>
#include <utility>

DiagramTab::DiagramTab(const std::string& name, IDatabaseNode* node)
    : Tab(name, TabType::DIAGRAM), node_(node) {
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

        // Check async loading status using IDatabaseNode interface
        if (node_) {
            node_->checkLoadingStatus();
            if (node_->isTablesLoaded()) {
                isLoadingSchema = false;
            }
        }
        return;
    }

    // Toolbar
    if (node_) {
        ImGui::Text("Database: %s", node_->getFullPath().c_str());
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

void DiagramTab::loadDatabaseSchema() {
    nodes.clear();
    links.clear();
    tableToNodeIdMap.clear();
    foreignKeyCache.clear();
    nextNodeId = 1000;
    nextLinkId = 10000;
    nextPinId = 100000;

    if (!node_) {
        schemaLoaded = true;
        return;
    }

    // Check if tables are loaded
    if (!node_->isTablesLoaded() && !node_->isLoadingTables()) {
        node_->startTablesLoadAsync();
    }

    // If tables are still loading, wait
    if (node_->isLoadingTables()) {
        schemaLoaded = false;
        return;
    }

    const std::vector<Table>& tables = node_->getTables();

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
        if (position.x > maxWidth) {
            position.x = 100;
            position.y += verticalSpacing;
        }
    }

    detectForeignKeys();

    schemaLoaded = true;
}

void DiagramTab::createTableNode(const Table& table, const ImVec2& position) {
    if (tableToNodeIdMap.contains(table.name)) {
        return;
    }

    DiagramNode node;
    node.id = ax::NodeEditor::NodeId(nextNodeId++);
    node.tableName = table.name;
    node.columns = table.columns;
    node.position = position;

    node.isPrimaryTable =
        std::ranges::any_of(table.columns, [](const Column& col) { return col.isPrimaryKey; });

    node.columnPinIds.clear();
    node.columnPinIds.resize(table.columns.size());
    for (size_t i = 0; i < table.columns.size(); ++i) {
        node.columnPinIds[i] = ax::NodeEditor::PinId(nextPinId++);
    }

    nodes.push_back(node);
    tableToNodeIdMap[table.name] = node.id;
}

void DiagramTab::renderNodes() {
    if (nodes.empty()) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();

    const ImVec4 primaryTableColor = colors.yellow;
    const ImVec4 normalTableColor = colors.text;
    const ImVec4 normalColumnColor = colors.subtext1;
    const ImVec4 foreignKeyColor = colors.blue;
    const ImVec4 typeColor = colors.subtext0;
    const ImVec4 notNullColor = colors.red;

    std::set<std::pair<std::string, std::string>> foreignKeyColumns;
    for (const auto& link : links) {
        foreignKeyColumns.insert({link.fromTable, link.fromColumn});
    }

    std::set<std::pair<std::string, std::string>> referencedColumns;
    for (const auto& link : links) {
        referencedColumns.insert({link.toTable, link.toColumn});
    }

    for (auto& [id, tableName, columns, position, isPrimaryTable, initialPositionSet,
                columnPinIds] : nodes) {
        if (!initialPositionSet) {
            ax::NodeEditor::SetNodePosition(id, position);
            initialPositionSet = true;
        }

        ax::NodeEditor::BeginNode(id);

        ImGui::PushStyleColor(ImGuiCol_Text, isPrimaryTable ? primaryTableColor : normalTableColor);
        ImGui::Text(ICON_FA_TABLE " %s", tableName.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();

        for (size_t i = 0; i < columns.size(); ++i) {
            const auto& column = columns[i];

            if (column.name.empty()) {
                continue;
            }

            const bool isForeignKey = foreignKeyColumns.contains({tableName, column.name});

            ImGui::BeginGroup();

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

            if (showPrimaryKeys && column.isPrimaryKey) {
                ImGui::PushStyleColor(ImGuiCol_Text, primaryTableColor);
                ImGui::Text(ICON_FA_KEY " %s", column.name.c_str());
                ImGui::PopStyleColor();
            } else if (isForeignKey && showForeignKeys) {
                ImGui::PushStyleColor(ImGuiCol_Text, foreignKeyColor);
                ImGui::Text(ICON_FA_LINK " %s", column.name.c_str());
                ImGui::PopStyleColor();

                if (ImGui::IsItemHovered()) {
                    std::string cacheKey = tableName + "." + column.name;
                    auto fkIt = foreignKeyCache.find(cacheKey);
                    if (fkIt != foreignKeyCache.end()) {
                        ImGui::SetTooltip("Foreign Key -> %s.%s", fkIt->second.first.c_str(),
                                          fkIt->second.second.c_str());
                    }
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, normalColumnColor);
                ImGui::Text("%s", column.name.c_str());
                ImGui::PopStyleColor();
            }

            if (showColumnTypes && !column.type.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, typeColor);
                ImGui::Text("(%s)", column.type.c_str());
                ImGui::PopStyleColor();
            }

            if (column.isNotNull && !column.isPrimaryKey) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, notNullColor);
                ImGui::Text("NOT NULL");
                ImGui::PopStyleColor();
            }

            ImGui::EndGroup();
        }

        ax::NodeEditor::EndNode();

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

    ax::NodeEditor::LinkId hoveredLinkId = ax::NodeEditor::GetHoveredLink();
    if (hoveredLinkId) {
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
    ax::NodeEditor::NodeId hoveredNodeId = ax::NodeEditor::GetHoveredNode();
    if (hoveredNodeId) {
        const auto nodeIt = std::ranges::find_if(
            nodes, [hoveredNodeId](const DiagramNode& node) { return node.id == hoveredNodeId; });

        if (nodeIt != nodes.end()) {
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Table: %s\nColumns: %zu", nodeIt->tableName.c_str(),
                                  nodeIt->columns.size());
            }
        }
    }

    ax::NodeEditor::NodeId contextNodeId;
    if (ax::NodeEditor::ShowNodeContextMenu(&contextNodeId)) {
        const auto nodeIt = std::ranges::find_if(
            nodes, [contextNodeId](const DiagramNode& node) { return node.id == contextNodeId; });

        if (nodeIt != nodes.end()) {
            ImGui::Text("Table: %s", nodeIt->tableName.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("View Data")) {
                // TODO: Implement table viewer opening
            }
            if (ImGui::MenuItem("New SQL Editor")) {
                // TODO: Implement SQL editor opening
            }
        }
    }
}

void DiagramTab::detectForeignKeys() {
    foreignKeyCache.clear();

    if (!node_) {
        return;
    }

    const std::vector<Table>& tables = node_->getTables();

    for (const auto& table : tables) {
        for (const auto& fk : table.foreignKeys) {
            std::string cacheKey = table.name + "." + fk.sourceColumn;
            foreignKeyCache[cacheKey] = {fk.targetTable, fk.targetColumn};

            auto sourceNodeIt = tableToNodeIdMap.find(table.name);
            if (sourceNodeIt == tableToNodeIdMap.end())
                continue;

            auto targetNodeIt = tableToNodeIdMap.find(fk.targetTable);
            if (targetNodeIt == tableToNodeIdMap.end())
                continue;

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

    if (links.empty()) {
        detectForeignKeysHeuristic();
    }
}

void DiagramTab::detectForeignKeysHeuristic() {
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
    const std::string suffix = "_id";
    if (columnName.length() > suffix.length() &&
        columnName.substr(columnName.length() - suffix.length()) == suffix) {
        const std::string potentialTable = columnName.substr(0, columnName.length() - 3);

        if (tableToNodeIdMap.contains(potentialTable + "s")) {
            referencedTable = potentialTable + "s";
            referencedColumn = "id";
            return true;
        }
        if (tableToNodeIdMap.contains(potentialTable)) {
            referencedTable = potentialTable;
            referencedColumn = "id";
            return true;
        }
    }

    for (const auto& table : tableToNodeIdMap | std::views::keys) {
        if (columnName == table + "_id") {
            referencedTable = table;
            referencedColumn = "id";
            return true;
        }
    }

    return false;
}
