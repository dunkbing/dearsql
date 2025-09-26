#pragma once

#include "database/db_interface.hpp"
#include "imgui_node_editor.h"
#include "ui/tab/tab.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

struct DiagramNode {
    ax::NodeEditor::NodeId id;
    std::string tableName;
    std::vector<Column> columns;
    ImVec2 position;
    bool isPrimaryTable = false;
    bool initialPositionSet = false;                 // Track if initial position has been set
    std::vector<ax::NodeEditor::PinId> columnPinIds; // Cache pin IDs for each column
};

struct DiagramLink {
    ax::NodeEditor::LinkId id;
    ax::NodeEditor::PinId startPinId;
    ax::NodeEditor::PinId endPinId;
    std::string fromTable;
    std::string toTable;
    std::string fromColumn;
    std::string toColumn;
};

class DiagramTab : public Tab {
public:
    DiagramTab(const std::string& name, std::shared_ptr<DatabaseInterface> database,
               std::string targetDatabaseName = "");
    ~DiagramTab() override;

    void render() override;

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
    [[nodiscard]] std::vector<Table>
    getTablesForDiagram() const; // Helper to get tables from database

private:
    std::shared_ptr<DatabaseInterface> database;
    std::string targetDatabaseName;
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
