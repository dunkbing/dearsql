#pragma once

#include "database/db.hpp"
#include "imgui_node_editor.h"
#include "ui/tab/tab.hpp"
#include <unordered_map>
#include <vector>

// Forward declarations
class IDatabaseNode;

struct DiagramNode {
    ax::NodeEditor::NodeId id;
    std::string tableName;
    std::vector<Column> columns;
    ImVec2 position;
    ImVec2 size = ImVec2(0, 0);
    bool isPrimaryTable = false;
    bool initialPositionSet = false;
    std::vector<ax::NodeEditor::PinId> columnPinIds;
    std::vector<float> columnPinCanvasY; // canvas-space Y center per column row
};

struct DiagramLink {
    ax::NodeEditor::LinkId id;
    ax::NodeEditor::PinId startPinId;
    ax::NodeEditor::PinId endPinId;
    std::string fromTable;
    std::string toTable;
    std::string fromColumn;
    std::string toColumn;
    float midXOffset = 0.0f; // horizontal offset for the middle vertical segment
    bool isDragging = false;
    float dragStartMouseX = 0.0f;
    float dragStartOffset = 0.0f;
};

class DiagramTab : public Tab {
public:
    DiagramTab(const std::string& name, IDatabaseNode* node);
    ~DiagramTab() override;

    void render() override;

    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
    }

private:
    void initializeEditor();
    void loadDatabaseSchema();
    void renderNodes();
    void renderLinks();
    void handleNodeInteraction();
    static void handleZoomShortcuts();
    void createTableNode(const Table& table, const ImVec2& position);

    // Foreign key detection methods
    void detectForeignKeys();
    void detectForeignKeysHeuristic(); // Fallback heuristic detection
    bool isForeignKeyColumn(const std::string& tableName, const std::string& columnName,
                            std::string& referencedTable, std::string& referencedColumn);

private:
    IDatabaseNode* node_ = nullptr;
    ax::NodeEditor::EditorContext* editorContext = nullptr;

    std::vector<DiagramNode> nodes;
    std::vector<DiagramLink> links;
    std::unordered_map<std::string, ax::NodeEditor::NodeId> tableToNodeIdMap;

    // Cache for foreign key relationships
    std::unordered_map<std::string, std::pair<std::string, std::string>> foreignKeyCache;

    int nextNodeId = 1000;
    int nextLinkId = 10000;
    int nextPinId = 100000;

    bool schemaLoaded = false;
    bool isLoadingSchema = false; // Track loading state per instance

    // UI state
    bool showTableDetails = true;
    bool showColumnTypes = true;
    bool showPrimaryKeys = true;
    bool showForeignKeys = true;
};
