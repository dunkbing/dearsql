#pragma once

#include <functional>
#include <string>
#include <vector>

struct TableColumn {
    std::string name;
    float width = 120.0f;
    bool resizable = true;
};

struct TableCell {
    std::string value;
    bool isEdited = false;
    bool isSelected = false;
};

enum class SortDirection { None, Ascending, Descending };

class TableRenderer {
public:
    struct Config {
        bool allowEditing = false;
        bool showRowNumbers = false;
        bool allowSelection = true;
        float minHeight = 50.0f;
        int tableFlags = 0; // ImGuiTableFlags
    };

    // Callbacks for editing functionality
    using OnCellEditCallback = std::function<void(int row, int col, const std::string& newValue)>;
    using OnCellSelectCallback = std::function<void(int row, int col)>;
    using OnCellDoubleClickCallback = std::function<void(int row, int col)>;
    using OnSortChangedCallback =
        std::function<void(int col, const std::string& colName, SortDirection direction)>;

    TableRenderer();
    explicit TableRenderer(const Config& config);

    // Set table data
    void setColumns(const std::vector<std::string>& columnNames);
    void setData(const std::vector<std::vector<std::string>>& tableData);
    void setCellEditedStatus(const std::vector<std::vector<bool>>& editedCells);
    void setSelectedCell(int row, int col);
    void setRowNumberOffset(int offset) {
        rowNumberOffset = offset;
    }

    // Set callbacks
    void setOnCellEdit(OnCellEditCallback callback) {
        onCellEdit = callback;
    }
    void setOnCellSelect(OnCellSelectCallback callback) {
        onCellSelect = callback;
    }
    void setOnCellDoubleClick(OnCellDoubleClickCallback callback) {
        onCellDoubleClick = callback;
    }
    void setOnSortChanged(OnSortChangedCallback callback) {
        onSortChanged = callback;
    }

    // Sorting
    void setSortColumn(int col, SortDirection direction) {
        sortColumn = col;
        sortDirection = direction;
    }
    [[nodiscard]] int getSortColumn() const {
        return sortColumn;
    }
    [[nodiscard]] SortDirection getSortDirection() const {
        return sortDirection;
    }

    // Render the table
    void render(const char* tableId = "##table");

    // Get current editing state
    bool isEditing() const {
        return editingRow >= 0 && editingCol >= 0;
    }
    int getEditingRow() const {
        return editingRow;
    }
    int getEditingCol() const {
        return editingCol;
    }

    // Control editing
    void enterEditMode(int row, int col);
    void exitEditMode(bool saveEdit = true);

    // Scrolling control
    void scrollToCell(int row, int col);

private:
    Config config;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> data;
    std::vector<std::vector<bool>> editedCells;

    int selectedRow = -1;
    int selectedCol = -1;
    int editingRow = -1;
    int editingCol = -1;
    int rowNumberOffset = 0;

    char editBuffer[1024] = {0};

    // Scrolling state
    bool shouldScrollToCell = false;
    int scrollTargetRow = -1;
    int scrollTargetCol = -1;

    // Edit mode state
    bool justEnteredEditWithChar = false;
    int initialCursorPos = 0;

    // Callbacks
    OnCellEditCallback onCellEdit;
    OnCellSelectCallback onCellSelect;
    OnCellDoubleClickCallback onCellDoubleClick;
    OnSortChangedCallback onSortChanged;

    // Sorting state
    int sortColumn = -1;
    SortDirection sortDirection = SortDirection::None;

    void renderCell(int row, int col);
    void handleCellInteraction(int row, int col, bool isSelected);
    void renderColumnHeader(int colIdx, const std::string& colName);
};
