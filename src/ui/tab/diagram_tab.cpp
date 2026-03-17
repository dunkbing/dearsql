#include "ui/tab/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <ranges>
#include <set>
#include <utility>

// Draws a 3-segment orthogonal path: p0 → (midX, p0.y) → (midX, p1.y) → p1
// with rounded corners of the given radius.
static void drawOrthogonalPath(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float midX, ImU32 color,
                               float thickness, float radius) {
    const float h1 = midX - p0.x;
    const float h2 = p1.x - midX;
    const float v = p1.y - p0.y;

    // degenerate: nearly straight horizontal
    if (std::abs(v) < 2.0f) {
        dl->AddLine(p0, p1, color, thickness);
        return;
    }

    const float sh1 = h1 >= 0.0f ? 1.0f : -1.0f;
    const float sh2 = h2 >= 0.0f ? 1.0f : -1.0f;
    const float sv = v >= 0.0f ? 1.0f : -1.0f;

    // clamp radius so it never exceeds half of any segment
    float r = std::min({radius, std::abs(h1), std::abs(h2), std::abs(v) * 0.5f});
    r = std::max(r, 0.0f);

    if (r < 0.5f) {
        dl->AddLine(p0, ImVec2(midX, p0.y), color, thickness);
        dl->AddLine(ImVec2(midX, p0.y), ImVec2(midX, p1.y), color, thickness);
        dl->AddLine(ImVec2(midX, p1.y), p1, color, thickness);
        return;
    }

    // corner 1: turn from h1 direction into vertical direction at (midX, p0.y)
    const ImVec2 c1 = {midX, p0.y};
    const ImVec2 c1i = {midX - sh1 * r, p0.y}; // enter corner 1
    const ImVec2 c1o = {midX, p0.y + sv * r};  // exit corner 1

    // corner 2: turn from vertical direction into h2 direction at (midX, p1.y)
    const ImVec2 c2 = {midX, p1.y};
    const ImVec2 c2i = {midX, p1.y - sv * r};  // enter corner 2
    const ImVec2 c2o = {midX + sh2 * r, p1.y}; // exit corner 2

    // Build path with cubic bezier corners (control points both at the corner vertex
    // gives a smooth quarter-circle approximation)
    dl->PathClear();
    dl->PathLineTo(p0);
    dl->PathLineTo(c1i);
    dl->PathBezierCubicCurveTo(c1, c1, c1o);
    dl->PathLineTo(c2i);
    dl->PathBezierCubicCurveTo(c2, c2, c2o);
    dl->PathLineTo(p1);
    dl->PathStroke(color, false, thickness);
}

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
        return;
    }

    ax::NodeEditor::SetCurrentEditor(editorContext);
    ax::NodeEditor::GetStyle().NodeRounding = 0.0f;
    ax::NodeEditor::SetCurrentEditor(nullptr);
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

    if (!editorContext) {
        std::cout << "DiagramTab: Editor context is null, cannot render!" << std::endl;
        return;
    }

    ax::NodeEditor::SetCurrentEditor(editorContext);

    handleZoomShortcuts();

    const std::string editorId =
        "Database Diagram##" + std::to_string(reinterpret_cast<uintptr_t>(this));
    ax::NodeEditor::Begin(editorId.c_str(), ImVec2(0.0, 0.0f));

    // Nodes (and pins) must be submitted BEFORE links — ax::NodeEditor::Link()
    // requires both pins to be registered in the current frame first.
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

    // Adaptive grid: number of columns ≈ ceil(sqrt(n)) for a roughly square layout
    const int tableCount = static_cast<int>(tables.size());
    const int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(tableCount)))));

    constexpr float startX = 80.0f;
    constexpr float startY = 80.0f;
    constexpr float horizontalSpacing = 380.0f;
    constexpr float verticalSpacing   = 320.0f;

    int col = 0, row = 0;
    for (const auto& table : tables) {
        const ImVec2 pos(startX + col * horizontalSpacing, startY + row * verticalSpacing);
        createTableNode(table, pos);
        ++col;
        if (col >= cols) {
            col = 0;
            ++row;
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

    node.columnPinIds.resize(table.columns.size());
    node.columnOutputPinIds.resize(table.columns.size());
    node.columnPinCanvasY.resize(table.columns.size(), 0.0f);
    for (size_t i = 0; i < table.columns.size(); ++i) {
        node.columnPinIds[i]       = ax::NodeEditor::PinId(nextPinId++); // Input pin
        node.columnOutputPinIds[i] = ax::NodeEditor::PinId(nextPinId++); // Output pin
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

    for (auto& node : nodes) {
        if (!node.initialPositionSet) {
            ax::NodeEditor::SetNodePosition(node.id, node.position);
            node.initialPositionSet = true;
        }

        // Update stored size (valid after the first rendered frame)
        node.size = ax::NodeEditor::GetNodeSize(node.id);

        ax::NodeEditor::BeginNode(node.id);

        ImGui::PushStyleColor(ImGuiCol_Text,
                              node.isPrimaryTable ? primaryTableColor : normalTableColor);
        ImGui::Text(ICON_FA_TABLE " %s", node.tableName.c_str());
        ImGui::PopStyleColor();

        // Capture the screen Y of the separator INSIDE BeginNode where ImGui cursor = screen space.
        node.separatorScreenY = ImGui::GetCursorScreenPos().y;
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        // Extra spacing between column rows without creating layout items
        const ImVec2 baseSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(baseSpacing.x, baseSpacing.y + 2.0f));

        for (size_t i = 0; i < node.columns.size(); ++i) {
            const auto& column = node.columns[i];

            if (column.name.empty()) {
                continue;
            }

            const bool isForeignKey = foreignKeyColumns.contains({node.tableName, column.name});

            ImGui::BeginGroup();

            ax::NodeEditor::BeginPin(node.columnPinIds[i], ax::NodeEditor::PinKind::Input);
            // Store Y in CANVAS space so renderLinks() can use CanvasToScreen consistently
            float screenY = ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
            node.columnPinCanvasY[i] = ax::NodeEditor::ScreenToCanvas({0.0f, screenY}).y;
            // 1px-wide placeholder: gives the pin a proper non-zero bounding rect
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
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
                    std::string cacheKey = node.tableName + "." + column.name;
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

            // Output pin on the right (FK source side)
            ImGui::SameLine();
            ax::NodeEditor::BeginPin(node.columnOutputPinIds[i], ax::NodeEditor::PinKind::Output);
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
            ax::NodeEditor::EndPin();

            ImGui::EndGroup();
        }

        ImGui::PopStyleVar();

        ax::NodeEditor::EndNode();



        node.position = ax::NodeEditor::GetNodePosition(node.id);
    }
}

void DiagramTab::renderLinks() {
    if (!showForeignKeys || links.empty()) {
        return;
    }

    const auto& colors = Application::getInstance().getCurrentColors();
    const ImVec4 linkColor = colors.sky;
    const ImVec4 linkHoverColor = colors.blue;
    constexpr float thickness = 2.5f;

    for (auto& link : links) {
        // Use ax::NodeEditor::Link() which handles all coordinate transforms internally.
        // startPinId = Output pin on the source column (FK referencing side)
        // endPinId   = Input pin on the target column (PK/referenced side)
        ax::NodeEditor::Link(link.id, link.startPinId, link.endPinId, linkColor, thickness);
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
                            // Use Output pin as start (FK source side)
                            sourcePinId = node.columnOutputPinIds[colIdx];
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
}
