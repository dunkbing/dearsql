#include "ui/tab/diagram_tab.hpp"
#include "IconsFontAwesome6.h"
#include "application.hpp"
#include "database/database_node.hpp"
#include "imgui.h"
#include "themes.hpp"
#include <algorithm>
#include <box2d/box2d.h>
#include <iostream>
#include <ranges>
#include <set>
#include <utility>

namespace {

    inline ImVec2 rotateLocalToWorld(float lx, float ly, b2Vec2 center, b2Rot rot) {
        return ImVec2(center.x + lx * rot.c - ly * rot.s, center.y + lx * rot.s + ly * rot.c);
    }

    inline void addLineRotated(ImDrawList* dl, float lx0, float ly0, float lx1, float ly1,
                               b2Vec2 center, b2Rot rot, ImU32 color, float thickness) {
        dl->AddLine(rotateLocalToWorld(lx0, ly0, center, rot),
                    rotateLocalToWorld(lx1, ly1, center, rot), color, thickness);
    }

    // Decode a single UTF-8 codepoint. Returns bytes consumed; on a malformed
    // lead byte, emits '?' and skips one byte so we never get stuck.
    inline int decodeUtf8(unsigned int* out, const char* s) {
        const unsigned char b0 = static_cast<unsigned char>(s[0]);
        if (b0 < 0x80) {
            *out = b0;
            return 1;
        }
        if ((b0 & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            *out = ((b0 & 0x1Fu) << 6) | (s[1] & 0x3Fu);
            return 2;
        }
        if ((b0 & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            *out = ((b0 & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
            return 3;
        }
        if ((b0 & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
            (s[3] & 0xC0) == 0x80) {
            *out = ((b0 & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) | ((s[2] & 0x3Fu) << 6) |
                   (s[3] & 0x3Fu);
            return 4;
        }
        *out = '?';
        return 1;
    }

    // Renders text by emitting one rotated textured quad per glyph. ImDrawList::AddText
    // can't rotate, so we drive the font atlas at the primitive level.
    // Returns the advance-x width of the rendered string in body-local units.
    // Glyph metrics are already in pixels at the bound size — no scaling needed.
    float addTextRotated(ImDrawList* dl, const char* text, float lx, float ly, b2Vec2 center,
                         b2Rot rot, ImU32 color) {
        ImFontBaked* baked = ImGui::GetFontBaked();
        if (!baked) {
            return 0.0f;
        }
        dl->PushTexture(ImGui::GetFont()->OwnerAtlas->TexRef);

        float cursorX = lx;
        const char* s = text;
        while (*s) {
            unsigned int cp = 0;
            const int len = decodeUtf8(&cp, s);
            if (len <= 0) {
                break;
            }
            s += len;

            const ImFontGlyph* glyph = baked->FindGlyph(static_cast<ImWchar>(cp));
            if (!glyph) {
                continue;
            }
            if (glyph->Visible) {
                const float gx0 = cursorX + glyph->X0;
                const float gy0 = ly + glyph->Y0;
                const float gx1 = cursorX + glyph->X1;
                const float gy1 = ly + glyph->Y1;
                const ImVec2 q0 = rotateLocalToWorld(gx0, gy0, center, rot);
                const ImVec2 q1 = rotateLocalToWorld(gx1, gy0, center, rot);
                const ImVec2 q2 = rotateLocalToWorld(gx1, gy1, center, rot);
                const ImVec2 q3 = rotateLocalToWorld(gx0, gy1, center, rot);
                dl->PrimReserve(6, 4);
                dl->PrimQuadUV(q0, q1, q2, q3, ImVec2(glyph->U0, glyph->V0),
                               ImVec2(glyph->U1, glyph->V0), ImVec2(glyph->U1, glyph->V1),
                               ImVec2(glyph->U0, glyph->V1), color);
            }
            cursorX += glyph->AdvanceX;
        }

        dl->PopTexture();
        return cursorX - lx;
    }

} // namespace

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

    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));

    // use a higher-contrast border against the diagram background
    const auto& themeColors = Application::getInstance().getCurrentColors();
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertFloat4ToU32(themeColors.overlay1));

    // Options + refresh button on a single row
    ImGui::Checkbox("Show Column Types", &showColumnTypes);
    ImGui::SameLine(0.0f, Theme::Spacing::L);
    ImGui::Checkbox("Show Primary Keys", &showPrimaryKeys);
    ImGui::SameLine(0.0f, Theme::Spacing::L);
    ImGui::Checkbox("Show Foreign Keys", &showForeignKeys);
    ImGui::SameLine(0.0f, Theme::Spacing::L);
    if (ImGui::Checkbox("Enable Physics", &enablePhysics)) {
        if (!enablePhysics) {
            teardownPhysicsWorld();
        }
    }
    ImGui::SameLine(0.0f, Theme::Spacing::L);
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE " Refresh")) {
        schemaLoaded = false;
        loadDatabaseSchema();
    }

    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, Theme::Spacing::S));

    if (!editorContext) {
        std::cout << "DiagramTab: Editor context is null, cannot render!" << std::endl;
        return;
    }

    ax::NodeEditor::SetCurrentEditor(editorContext);

    handleZoomShortcuts();

    // Capture the canvas bounds so we can stroke a border around it after
    // rendering, without introducing a child window/scroll container.
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    const std::string editorId =
        "Database Diagram##" + std::to_string(reinterpret_cast<uintptr_t>(this));

    // In physics mode the editor's hit test (axis-aligned) misses rotated nodes,
    // so a drag on a tumbled box looks like a drag on empty canvas and the editor
    // draws its blue marquee. Hide the marquee colors while physics is active.
    const bool hidingSelection = enablePhysics;
    if (hidingSelection) {
        const ImVec4 transparent(0.0f, 0.0f, 0.0f, 0.0f);
        ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeSelRect, transparent);
        ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_NodeSelRectBorder, transparent);
        ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_LinkSelRect, transparent);
        ax::NodeEditor::PushStyleColor(ax::NodeEditor::StyleColor_LinkSelRectBorder, transparent);
    }

    ax::NodeEditor::Begin(editorId.c_str(), canvasSize);

    ensurePhysicsBuilt();
    stepAndApplyPhysics();

    if (enablePhysics && physicsBuilt) {
        // After Begin() the editor leaves us on UserChannel_Content (idx 1),
        // rendered *before* the canvas Bg overlay on UserChannel_Grid (idx 2).
        // Switch to UserChannel_Hints (idx 4) so fills/text land on top of the
        // canvas Bg instead of being washed out under it.
        auto* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSetCurrent(4);
        renderGround();
        renderPhysicsView();
        dl->ChannelsSetCurrent(1);
    } else {
        renderGround();
        renderLinks();
        renderNodes();
        handleNodeInteraction();
    }

    ax::NodeEditor::End();

    if (hidingSelection) {
        ax::NodeEditor::PopStyleColor(4);
    }

    ax::NodeEditor::SetCurrentEditor(nullptr);

    const ImVec2 canvasMax = {canvasMin.x + canvasSize.x, canvasMin.y + canvasSize.y};
    ImGui::GetWindowDrawList()->AddRect(
        canvasMin, canvasMax, ImGui::ColorConvertFloat4ToU32(themeColors.overlay1), 0.0f, 0, 1.0f);
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
    teardownPhysicsWorld();
    nodes.clear();
    links.clear();
    tableToNodeIdMap.clear();
    foreignKeyCache.clear();
    selectedLinkIndex = -1;
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
    const ImVec4 selectedColumnColor = colors.peach;

    std::set<std::pair<std::string, std::string>> foreignKeyColumns;
    for (const auto& link : links) {
        foreignKeyColumns.insert({link.fromTable, link.fromColumn});
    }

    // Columns connected by the currently selected link — highlighted in renderNodes.
    std::set<std::pair<std::string, std::string>> selectedEndpoints;
    if (selectedLinkIndex >= 0 && selectedLinkIndex < static_cast<int>(links.size())) {
        const auto& sel = links[selectedLinkIndex];
        selectedEndpoints.insert({sel.fromTable, sel.fromColumn});
        selectedEndpoints.insert({sel.toTable, sel.toColumn});
    }
    ImVec4 selectedBgVec = selectedColumnColor;
    selectedBgVec.w = 0.18f;
    const ImU32 selectedBgColor = ImGui::ColorConvertFloat4ToU32(selectedBgVec);

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

        // Custom separator constrained to the node's width — ImGui::Separator()
        // would stretch across the canvas inside a node-editor node.
        {
            const auto& neStyle = ax::NodeEditor::GetStyle();
            const float titleWidth = ImGui::GetItemRectSize().x;
            const float innerWidth =
                node.size.x > 0.0f ? node.size.x - neStyle.NodePadding.x - neStyle.NodePadding.z
                                   : titleWidth;
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const float y = cursor.y + ImGui::GetStyle().ItemSpacing.y * 0.5f;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(cursor.x, y),
                                                ImVec2(cursor.x + innerWidth, y),
                                                ImGui::GetColorU32(ImGuiCol_Separator));
            ImGui::Dummy(ImVec2(innerWidth, ImGui::GetStyle().ItemSpacing.y));
        }

        // Extra spacing between column rows without creating layout items
        const ImVec2 baseSpacing = ImGui::GetStyle().ItemSpacing;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(baseSpacing.x, baseSpacing.y + 2.0f));

        for (size_t i = 0; i < node.columns.size(); ++i) {
            const auto& column = node.columns[i];

            if (column.name.empty()) {
                continue;
            }

            const bool isForeignKey = foreignKeyColumns.contains({node.tableName, column.name});
            const bool isSelectedEndpoint =
                selectedEndpoints.contains({node.tableName, column.name});

            ImGui::BeginGroup();

            ax::NodeEditor::BeginPin(node.columnPinIds[i], ax::NodeEditor::PinKind::Input);
            node.columnPinCanvasY[i] =
                ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f;
            // 1px-wide placeholder: gives the pin a proper non-zero bounding rect
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
            ax::NodeEditor::EndPin();

            ImGui::SameLine();

            if (isSelectedEndpoint) {
                ImGui::PushStyleColor(ImGuiCol_Text, selectedColumnColor);
                const char* icon = column.isPrimaryKey ? ICON_FA_KEY " "
                                   : isForeignKey      ? ICON_FA_LINK " "
                                                       : "";
                ImGui::Text("%s%s", icon, column.name.c_str());
                ImGui::PopStyleColor();
            } else if (showPrimaryKeys && column.isPrimaryKey) {
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

            // Translucent background tint behind a selected-endpoint row.
            // Drawn with low alpha so it sits behind the text visually.
            if (isSelectedEndpoint) {
                const ImVec2 rowMin = ImGui::GetItemRectMin();
                const ImVec2 rowMax = ImGui::GetItemRectMax();
                const float padX = ImGui::GetStyle().ItemSpacing.x * 0.5f;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(rowMin.x - padX, rowMin.y),
                                                          ImVec2(rowMax.x + padX, rowMax.y),
                                                          selectedBgColor, 3.0f);
            }
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
    const ImU32 linkSelectedColor = ImGui::ColorConvertFloat4ToU32(colors.peach);
    ImVec4 selectedGlow = colors.peach;
    selectedGlow.w = 0.30f;
    const ImU32 linkSelectedGlow = ImGui::ColorConvertFloat4ToU32(selectedGlow);
    constexpr float thickness = 2.5f;
    constexpr float cornerRadius = 8.0f;
    constexpr float hoverThresh = 6.0f;

    auto* drawList = ImGui::GetWindowDrawList();
    // Inside ax::NodeEditor::Begin/End, io.MousePos is already in canvas-local space.
    const ImVec2 mouseCanvas = ImGui::GetMousePos();
    bool anyDragActive = false;
    bool clickedAnyLink = false;

    if (selectedLinkIndex >= static_cast<int>(links.size())) {
        selectedLinkIndex = -1;
    }

    for (size_t linkIdx = 0; linkIdx < links.size(); ++linkIdx) {
        auto& link = links[linkIdx];
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
                selectedLinkIndex = static_cast<int>(linkIdx);
                clickedAnyLink = true;
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
        const bool isSelected = (static_cast<int>(linkIdx) == selectedLinkIndex);

        ImU32 color;
        float lw;
        if (isSelected) {
            color = linkSelectedColor;
            lw = thickness + 2.0f;
            // Soft outer glow underneath the main line.
            drawOrthogonalPath(drawList, startPin, endPin, midX, linkSelectedGlow, lw + 4.0f,
                               cornerRadius);
        } else if (isHovered || link.isDragging) {
            color = linkHoverColor;
            lw = thickness + 1.0f;
        } else {
            color = linkColor;
            lw = thickness;
        }

        drawOrthogonalPath(drawList, startPin, endPin, midX, color, lw, cornerRadius);

        // Tooltip on hover — positioned near the cursor with a visible border.
        // We Suspend() the editor's coordinate transform so the popup uses
        // screen-space coordinates and ImGui places it correctly.
        if (isHovered) {
            ax::NodeEditor::Suspend();
            const ImVec2 mouseScreen = ImGui::GetIO().MousePos;
            ImGui::SetNextWindowPos(ImVec2(mouseScreen.x + 16.0f, mouseScreen.y + 16.0f),
                                    ImGuiCond_Always);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.5f);
            ImGui::PushStyleColor(ImGuiCol_Border, color);
            if (ImGui::BeginTooltip()) {
                ImGui::Text("%s.%s", link.fromTable.c_str(), link.fromColumn.c_str());
                ImGui::Text("  ↓");
                ImGui::Text("%s.%s", link.toTable.c_str(), link.toColumn.c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
            ax::NodeEditor::Resume();
        }
    }

    // Click on empty canvas (no link, no node) clears the link selection.
    if (ImGui::IsMouseClicked(0) && !clickedAnyLink && !ax::NodeEditor::GetHoveredNode() &&
        !ax::NodeEditor::GetHoveredPin()) {
        selectedLinkIndex = -1;
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

void DiagramTab::buildPhysicsWorld() {
    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = b2Vec2{0.0f, 1200.0f}; // +Y is down in canvas space
    physicsWorld = b2CreateWorld(&worldDef);

    // Ground sits a comfortable distance below the lowest spawn position so all
    // tables get a visible drop before piling up.
    float maxBottom = 0.0f;
    for (auto& node : nodes) {
        const float h = node.size.y > 0.0f ? node.size.y : 200.0f;
        maxBottom = std::max(maxBottom, node.position.y + h);
    }
    groundY = maxBottom + 500.0f;

    {
        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = b2_staticBody;
        bd.position = b2Vec2{0.0f, groundY + 50.0f};
        groundBody = b2CreateBody(physicsWorld, &bd);
        b2Polygon groundBox = b2MakeBox(20000.0f, 50.0f);
        b2ShapeDef sd = b2DefaultShapeDef();
        sd.material.friction = 0.7f;
        b2CreatePolygonShape(groundBody, &sd, &groundBox);
    }

    for (auto& node : nodes) {
        const float w = node.size.x > 0.0f ? node.size.x : 200.0f;
        const float h = node.size.y > 0.0f ? node.size.y : 200.0f;
        const ImVec2 cur = ax::NodeEditor::GetNodePosition(node.id);

        b2BodyDef bodyDef = b2DefaultBodyDef();
        bodyDef.type = b2_dynamicBody;
        bodyDef.position = b2Vec2{cur.x + w * 0.5f, cur.y + h * 0.5f};
        bodyDef.linearDamping = 0.05f;
        bodyDef.angularDamping = 0.4f;
        node.physicsBody = b2CreateBody(physicsWorld, &bodyDef);

        b2Polygon box = b2MakeBox(w * 0.5f, h * 0.5f);
        b2ShapeDef shapeDef = b2DefaultShapeDef();
        shapeDef.density = 1.0f;
        shapeDef.material.friction = 0.4f;
        shapeDef.material.restitution = 0.25f;
        b2CreatePolygonShape(node.physicsBody, &shapeDef, &box);
    }

    for (const auto& link : links) {
        auto fromIt = std::ranges::find_if(
            nodes, [&](const DiagramNode& n) { return n.tableName == link.fromTable; });
        auto toIt = std::ranges::find_if(
            nodes, [&](const DiagramNode& n) { return n.tableName == link.toTable; });
        if (fromIt == nodes.end() || toIt == nodes.end())
            continue;
        if (!b2Body_IsValid(fromIt->physicsBody) || !b2Body_IsValid(toIt->physicsBody))
            continue;

        b2DistanceJointDef jointDef = b2DefaultDistanceJointDef();
        jointDef.bodyIdA = fromIt->physicsBody;
        jointDef.bodyIdB = toIt->physicsBody;
        jointDef.localAnchorA = b2Vec2{0.0f, 0.0f};
        jointDef.localAnchorB = b2Vec2{0.0f, 0.0f};
        jointDef.length = 350.0f;
        jointDef.minLength = 100.0f;
        jointDef.maxLength = 1500.0f;
        jointDef.enableSpring = true;
        jointDef.hertz = 2.0f;
        jointDef.dampingRatio = 0.4f;
        jointDef.enableLimit = true;
        b2CreateDistanceJoint(physicsWorld, &jointDef);
    }

    physicsBuilt = true;
}

void DiagramTab::teardownPhysicsWorld() {
    if (b2World_IsValid(physicsWorld)) {
        b2DestroyWorld(physicsWorld);
    }
    physicsWorld = b2WorldId{};
    groundBody = b2BodyId{};
    groundY = 0.0f;
    physicsBuilt = false;
    draggedNodeId = ax::NodeEditor::NodeId{};
    for (auto& node : nodes) {
        node.physicsBody = b2BodyId{};
    }
}

void DiagramTab::ensurePhysicsBuilt() {
    if (!enablePhysics || physicsBuilt || nodes.empty()) {
        return;
    }
    // Need rendered sizes before we can build the rectangular bodies
    for (const auto& node : nodes) {
        if (node.size.x <= 0.0f || node.size.y <= 0.0f) {
            return;
        }
    }
    buildPhysicsWorld();
}

void DiagramTab::stepAndApplyPhysics() {
    if (!enablePhysics || !physicsBuilt || !b2World_IsValid(physicsWorld)) {
        return;
    }

    const bool mouseDown = ImGui::IsMouseDown(0);
    const ImVec2 mouseCanvas = ImGui::GetMousePos();

    // Click-time hit test against each rotated body so we can grab tumbled boxes
    // (the editor's own hit testing is axis-aligned and would miss them).
    if (ImGui::IsMouseClicked(0)) {
        for (auto& node : nodes) {
            if (!b2Body_IsValid(node.physicsBody))
                continue;
            const b2Vec2 c = b2Body_GetPosition(node.physicsBody);
            const b2Rot rot = b2Body_GetRotation(node.physicsBody);
            const float hw = node.size.x * 0.5f;
            const float hh = node.size.y * 0.5f;
            const float dx = mouseCanvas.x - c.x;
            const float dy = mouseCanvas.y - c.y;
            // R^-1 * (world - center) → body-local point
            const float lx = dx * rot.c + dy * rot.s;
            const float ly = -dx * rot.s + dy * rot.c;
            if (std::abs(lx) <= hw && std::abs(ly) <= hh) {
                draggedNodeId = node.id;
                dragOffsetX = dx;
                dragOffsetY = dy;
                break;
            }
        }
    }
    if (!mouseDown) {
        draggedNodeId = ax::NodeEditor::NodeId{};
    }

    const float frameDt = std::max(ImGui::GetIO().DeltaTime, 1.0f / 240.0f);
    constexpr float kMaxThrowVel = 6000.0f;

    for (auto& node : nodes) {
        if (!b2Body_IsValid(node.physicsBody))
            continue;
        const bool dragged = mouseDown && node.id == draggedNodeId;
        if (!dragged)
            continue;
        // Keep the click point pinned under the cursor so the box doesn't snap-center.
        const b2Vec2 newCenter{mouseCanvas.x - dragOffsetX, mouseCanvas.y - dragOffsetY};
        const b2Vec2 oldBodyPos = b2Body_GetPosition(node.physicsBody);
        const b2Rot oldRot = b2Body_GetRotation(node.physicsBody);
        b2Vec2 vel{(newCenter.x - oldBodyPos.x) / frameDt, (newCenter.y - oldBodyPos.y) / frameDt};
        const float velMag = std::sqrt(vel.x * vel.x + vel.y * vel.y);
        if (velMag > kMaxThrowVel) {
            const float s = kMaxThrowVel / velMag;
            vel.x *= s;
            vel.y *= s;
        }
        b2Body_SetTransform(node.physicsBody, newCenter, oldRot);
        b2Body_SetLinearVelocity(node.physicsBody, vel);
        b2Body_SetAngularVelocity(node.physicsBody, 0.0f); // freeze spin while held
        b2Body_SetAwake(node.physicsBody, true);
    }

    constexpr float dt = 1.0f / 60.0f;
    b2World_Step(physicsWorld, dt, 4);

    for (auto& node : nodes) {
        if (!b2Body_IsValid(node.physicsBody))
            continue;
        const bool dragged = mouseDown && node.id == draggedNodeId;
        if (dragged)
            continue;
        const b2Vec2 p = b2Body_GetPosition(node.physicsBody);
        const ImVec2 newPos(p.x - node.size.x * 0.5f, p.y - node.size.y * 0.5f);
        // Sync the editor's position so toggling physics off leaves nodes where
        // they ended up (we ignore rotation here since the editor renders upright).
        ax::NodeEditor::SetNodePosition(node.id, newPos);
        node.position = newPos;
    }
}

void DiagramTab::renderGround() {
    if (!enablePhysics || !physicsBuilt) {
        return;
    }
    auto* dl = ImGui::GetWindowDrawList();
    const auto& colors = Application::getInstance().getCurrentColors();
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32(colors.surface1);
    const ImU32 line = ImGui::ColorConvertFloat4ToU32(colors.overlay2);
    const float left = -10000.0f;
    const float right = 10000.0f;
    const float top = groundY;
    const float bottom = groundY + 100.0f;
    dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), fill);
    dl->AddLine(ImVec2(left, top), ImVec2(right, top), line, 3.0f);
}

void DiagramTab::renderPhysicsView() {
    if (!enablePhysics || !physicsBuilt) {
        return;
    }
    auto* dl = ImGui::GetWindowDrawList();
    const auto& colors = Application::getInstance().getCurrentColors();

    // The editor draws its NodeBg on a back draw channel that composites over the
    // canvas Bg, so its 200/255 alpha visually reads as solid. Our physics quads
    // hit a different point in the draw order and the same alpha reads as washed
    // out, so we pre-blend NodeBg over Bg here and emit fully-opaque fill/border.
    const auto& neStyle = ax::NodeEditor::GetStyle();
    auto preBlend = [](ImVec4 fg, ImVec4 bg) {
        const float a = fg.w;
        return ImVec4(fg.x * a + bg.x * (1.0f - a), fg.y * a + bg.y * (1.0f - a),
                      fg.z * a + bg.z * (1.0f - a), 1.0f);
    };
    const ImVec4 canvasBg = neStyle.Colors[ax::NodeEditor::StyleColor_Bg];
    const ImVec4 nodeBg = neStyle.Colors[ax::NodeEditor::StyleColor_NodeBg];
    const ImVec4 nodeBorder = neStyle.Colors[ax::NodeEditor::StyleColor_NodeBorder];
    const ImU32 linkColor = ImGui::ColorConvertFloat4ToU32(colors.sky);
    const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(preBlend(nodeBg, canvasBg));
    const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(preBlend(nodeBorder, canvasBg));
    const ImU32 borderActiveColor = ImGui::ColorConvertFloat4ToU32(colors.peach);
    const ImU32 separatorColor = ImGui::ColorConvertFloat4ToU32(colors.overlay0);
    const ImU32 primaryColor = ImGui::ColorConvertFloat4ToU32(colors.yellow);
    const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(colors.text);
    const ImU32 columnColor = ImGui::ColorConvertFloat4ToU32(colors.subtext1);
    const ImU32 fkColor = ImGui::ColorConvertFloat4ToU32(colors.blue);
    const ImU32 typeColor = ImGui::ColorConvertFloat4ToU32(colors.subtext0);
    const ImU32 notNullColor = ImGui::ColorConvertFloat4ToU32(colors.red);

    // FK springs — connect body centers with a straight line
    for (const auto& link : links) {
        auto fromIt = std::ranges::find_if(
            nodes, [&](const DiagramNode& n) { return n.tableName == link.fromTable; });
        auto toIt = std::ranges::find_if(
            nodes, [&](const DiagramNode& n) { return n.tableName == link.toTable; });
        if (fromIt == nodes.end() || toIt == nodes.end())
            continue;
        if (!b2Body_IsValid(fromIt->physicsBody) || !b2Body_IsValid(toIt->physicsBody))
            continue;
        const b2Vec2 a = b2Body_GetPosition(fromIt->physicsBody);
        const b2Vec2 b = b2Body_GetPosition(toIt->physicsBody);
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), linkColor, 2.0f);
    }

    // FK source columns — for column tinting (matches the non-physics renderer)
    std::set<std::pair<std::string, std::string>> fkColumns;
    for (const auto& link : links) {
        fkColumns.insert({link.fromTable, link.fromColumn});
    }

    const float lineH = ImGui::GetTextLineHeight();
    constexpr float padX = 12.0f;
    constexpr float padY = 8.0f;
    constexpr float rowSpacing = 2.0f;

    for (const auto& node : nodes) {
        if (!b2Body_IsValid(node.physicsBody))
            continue;
        const b2Vec2 c = b2Body_GetPosition(node.physicsBody);
        const b2Rot rot = b2Body_GetRotation(node.physicsBody);
        const float hw = node.size.x * 0.5f;
        const float hh = node.size.y * 0.5f;

        const ImVec2 p0 = rotateLocalToWorld(-hw, -hh, c, rot);
        const ImVec2 p1 = rotateLocalToWorld(hw, -hh, c, rot);
        const ImVec2 p2 = rotateLocalToWorld(hw, hh, c, rot);
        const ImVec2 p3 = rotateLocalToWorld(-hw, hh, c, rot);
        const bool isDragged = node.id == draggedNodeId;
        dl->AddQuadFilled(p0, p1, p2, p3, fillColor);
        dl->AddQuad(p0, p1, p2, p3, isDragged ? borderActiveColor : borderColor, 2.0f);

        // Lay out the table content in body-local space (y grows downward, origin
        // at the box center). Every text/line call is rotated about the body center.
        float cursorY = -hh + padY;

        const std::string title = std::string(ICON_FA_TABLE " ") + node.tableName;
        addTextRotated(dl, title.c_str(), -hw + padX, cursorY, c, rot,
                       node.isPrimaryTable ? primaryColor : textColor);
        cursorY += lineH + 4.0f;

        addLineRotated(dl, -hw + padX, cursorY, hw - padX, cursorY, c, rot, separatorColor, 1.0f);
        cursorY += 6.0f;

        for (const auto& column : node.columns) {
            if (column.name.empty())
                continue;
            if (cursorY + lineH > hh - padY)
                break;

            const bool isFK = fkColumns.contains({node.tableName, column.name});
            std::string nameText;
            ImU32 nameColor;
            if (showPrimaryKeys && column.isPrimaryKey) {
                nameText = std::string(ICON_FA_KEY " ") + column.name;
                nameColor = primaryColor;
            } else if (showForeignKeys && isFK) {
                nameText = std::string(ICON_FA_LINK " ") + column.name;
                nameColor = fkColor;
            } else {
                nameText = column.name;
                nameColor = columnColor;
            }

            float xCursor = -hw + padX;
            xCursor += addTextRotated(dl, nameText.c_str(), xCursor, cursorY, c, rot, nameColor);

            if (showColumnTypes && !column.type.empty()) {
                const std::string typeStr = " (" + column.type + ")";
                xCursor += addTextRotated(dl, typeStr.c_str(), xCursor, cursorY, c, rot, typeColor);
            }
            if (column.isNotNull && !column.isPrimaryKey) {
                addTextRotated(dl, " NOT NULL", xCursor, cursorY, c, rot, notNullColor);
            }

            cursorY += lineH + rowSpacing;
        }
    }
}
