#pragma once

#include "database/db_interface.hpp"
#include "tab.hpp"
#include <imgui_node_editor.h>
#include <memory>
#include <unordered_map>
#include <vector>

struct DiagramNode {
    ax::NodeEditor::NodeId id;
    std::string tableName;
    std::vector<Column> columns;
    ImVec2 position;
    bool isPrimaryTable = false;
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
               const std::string& targetDatabaseName = "");
    ~DiagramTab() override;

    void render() override;

private:
    void initializeEditor();
    void loadDatabaseSchema();
    void renderNodes();
    void renderLinks();
    void handleNodeInteraction();
    void createTableNode(const Table& table, const ImVec2& position);
    void autoLayoutNodes();

    // Foreign key detection methods
    void detectForeignKeys();
    bool isForeignKeyColumn(const std::string& tableName, const std::string& columnName,
                            std::string& referencedTable, std::string& referencedColumn);

private:
    std::shared_ptr<DatabaseInterface> database;
    std::string targetDatabaseName;
    ax::NodeEditor::EditorContext* editorContext = nullptr;

    std::vector<DiagramNode> nodes;
    std::vector<DiagramLink> links;
    std::unordered_map<std::string, ax::NodeEditor::NodeId> tableToNodeId;

    int nextNodeId = 1;
    int nextLinkId = 1;
    int nextPinId = 1;

    bool schemaLoaded = false;
    bool autoLayout = true;
    float nodeSpacing = 300.0f;

    // UI state
    bool showTableDetails = true;
    bool showColumnTypes = true;
    bool showPrimaryKeys = true;
    bool showForeignKeys = true;
};
