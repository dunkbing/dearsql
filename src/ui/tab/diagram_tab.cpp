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

    // Links drawn first so they land on the background draw channel (below nodes)
    renderLinks();
    renderNodes();
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

    node.columnPinIds.resize(table.columns.size());
    node.columnPinCanvasY.resize(table.columns.size(), 0.0f);
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
        ImGui::Separator();

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
            node.columnPinCanvasY[i] =
                ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
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
    const ImU32 linkColor = ImGui::ColorConvertFloat4ToU32(colors.sky);
    const ImU32 linkHoverColor = ImGui::ColorConvertFloat4ToU32(colors.blue);
    constexpr float thickness = 2.5f;
    constexpr float cornerRadius = 8.0f;
    constexpr float hoverThresh = 6.0f;

    auto* drawList = ImGui::GetWindowDrawList();
    // Inside ax::NodeEditor::Begin/End, io.MousePos is already in canvas-local space.
    const ImVec2 mouseCanvas = ImGui::GetMousePos();
    bool anyDragActive = false;

    for (auto& link : links) {
        // Find nodes
        auto fromIt = std::ranges::find_if(
            nodes, [&](const DiagramNode& n) { return n.tableName == link.fromTable; });
        auto toIt = std::ranges::find_if(
            nodes, [&](const DiagramNode& n) { return n.tableName == link.toTable; });
        if (fromIt == nodes.end() || toIt == nodes.end())
            continue;

        // Find column indices
        int fromColIdx = -1, toColIdx = -1;
        for (size_t i = 0; i < fromIt->columns.size(); ++i) {
            if (fromIt->columns[i].name == link.fromColumn) {
                fromColIdx = static_cast<int>(i);
                break;
            }
        }
        for (size_t i = 0; i < toIt->columns.size(); ++i) {
            if (toIt->columns[i].name == link.toColumn) {
                toColIdx = static_cast<int>(i);
                break;
            }
        }
        if (fromColIdx < 0 || toColIdx < 0)
            continue;

        // Skip until canvas Y positions are captured (after first render frame)
        if (fromIt->columnPinCanvasY[fromColIdx] == 0.0f ||
            toIt->columnPinCanvasY[toColIdx] == 0.0f)
            continue;

        const ImVec2 fromPos = ax::NodeEditor::GetNodePosition(fromIt->id);
        const ImVec2 fromSize = ax::NodeEditor::GetNodeSize(fromIt->id);
        const ImVec2 toPos = ax::NodeEditor::GetNodePosition(toIt->id);
        const ImVec2 toSize = ax::NodeEditor::GetNodeSize(toIt->id);

        // Skip if node sizes haven't been computed yet
        if (fromSize.x == 0.0f || toSize.x == 0.0f)
            continue;

        // Determine which node edge to attach to based on horizontal positions.
        // The start pin exits from whichever side faces the other node.
        const float fromCenterX = fromPos.x + fromSize.x * 0.5f;
        const float toCenterX = toPos.x + toSize.x * 0.5f;
        const bool fromRight = (toCenterX > fromCenterX);

        const float startX = fromRight ? (fromPos.x + fromSize.x) : fromPos.x;
        const float endX = fromRight ? toPos.x : (toPos.x + toSize.x);

        const ImVec2 startPin = {startX, fromIt->columnPinCanvasY[fromColIdx]};
        const ImVec2 endPin = {endX, toIt->columnPinCanvasY[toColIdx]};

        // midX: centre between the two pins, adjusted by user drag offset
        float midX = (startPin.x + endPin.x) * 0.5f + link.midXOffset;

        // Clamp midX to stay in the gap between the two node edges (with a small margin)
        const float gapLeft = std::min(startPin.x, endPin.x) + 20.0f;
        const float gapRight = std::max(startPin.x, endPin.x) - 20.0f;
        if (gapLeft < gapRight) {
            midX = std::clamp(midX, gapLeft, gapRight);
        }

        // --- Hover & drag detection on the middle vertical segment ---
        const float vSegYMin = std::min(startPin.y, endPin.y);
        const float vSegYMax = std::max(startPin.y, endPin.y);

        // Hover detection on all three segments — dragging any of them moves midX
        const bool isHoveringVSeg =
            !anyDragActive && (std::abs(mouseCanvas.x - midX) < hoverThresh) &&
            (mouseCanvas.y >= vSegYMin - hoverThresh) && (mouseCanvas.y <= vSegYMax + hoverThresh);

        const bool isHoveringSeg1 = !anyDragActive &&
                                    (std::abs(mouseCanvas.y - startPin.y) < hoverThresh) &&
                                    (mouseCanvas.x >= std::min(startPin.x, midX) - hoverThresh) &&
                                    (mouseCanvas.x <= std::max(startPin.x, midX) + hoverThresh);

        const bool isHoveringSeg3 = !anyDragActive &&
                                    (std::abs(mouseCanvas.y - endPin.y) < hoverThresh) &&
                                    (mouseCanvas.x >= std::min(midX, endPin.x) - hoverThresh) &&
                                    (mouseCanvas.x <= std::max(midX, endPin.x) + hoverThresh);

        const bool isHovered = isHoveringVSeg || isHoveringSeg1 || isHoveringSeg3;

        // Update drag state — any segment drag moves midX horizontally
        if (link.isDragging) {
            anyDragActive = true;
            if (ImGui::IsMouseDown(0)) {
                link.midXOffset = link.dragStartOffset + (mouseCanvas.x - link.dragStartMouseX);
            } else {
                link.isDragging = false;
            }
        } else if (isHovered && ImGui::IsMouseClicked(0)) {
            // Only start drag if no node is hovered (avoid conflict with node dragging)
            if (!ax::NodeEditor::GetHoveredNode()) {
                link.isDragging = true;
                link.dragStartMouseX = mouseCanvas.x;
                link.dragStartOffset = link.midXOffset;
                anyDragActive = true;
            }
        }

        if (isHovered || link.isDragging) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        // Suppress the editor's selection rect while dragging a link segment
        if (link.isDragging) {
            ax::NodeEditor::ClearSelection();
        }

        // --- Draw the orthogonal link ---
        const bool highlight = isHovered || link.isDragging;
        const ImU32 color = highlight ? linkHoverColor : linkColor;
        const float lw = highlight ? thickness + 1.0f : thickness;

        drawOrthogonalPath(drawList, startPin, endPin, midX, color, lw, cornerRadius);

        // Tooltip on hover
        if (isHovered) {
            if (ImGui::BeginTooltip()) {
                ImGui::Text("%s.%s", link.fromTable.c_str(), link.fromColumn.c_str());
                ImGui::Text("  ↓");
                ImGui::Text("%s.%s", link.toTable.c_str(), link.toColumn.c_str());
                ImGui::EndTooltip();
            }
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
