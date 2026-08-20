#pragma once

#include "database/db.hpp"
#include "ui/datetime_picker.hpp"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

enum class SortDirection { None, Ascending, Descending };

class TableRenderer {
public:
    struct Config {
        bool allowEditing = false;
        bool showRowNumbers = false;
        bool allowSelection = true;
        float minHeight = 50.0f;
        int tableFlags = 0; // ImGuiTableFlags
        std::set<int> nonEditableColumns;
        std::map<int, int> columnInputFlags; // per-column ImGuiInputTextFlags
        std::map<int, std::vector<std::string>> columnDropdownOptions;
    };

    // Callbacks for editing functionality
    using OnCellEditCallback = std::function<void(int row, int col, const std::string& newValue)>;
    using OnCellSelectCallback = std::function<void(int row, int col)>;
    using OnCellDoubleClickCallback = std::function<void(int row, int col)>;
    using OnSortChangedCallback =
        std::function<void(int col, const std::string& colName, SortDirection direction)>;
    // returns optional color override for a cell (0 = no override)
    using CellColorCallback =
        std::function<unsigned int(int row, int col, const std::string& value)>;
    // returns false to block editing a specific cell
    using CellEditableCallback = std::function<bool(int row, int col)>;
    // returns true if a column is nullable (enables "Set NULL" context menu)
    using ColumnNullableCallback = std::function<bool(int col)>;
    // called when user selects "Set NULL" from context menu
    using OnSetNullCallback = std::function<void(int row, int col)>;
    using OnFilterByValueCallback = std::function<void(int row, int col, const std::string& value)>;
    using OnDeleteRowCallback = std::function<void(int row)>;

    TableRenderer();
    explicit TableRenderer(const Config& config);

    // Set table data
    void setColumns(const std::vector<Column>& cols);
    void setColumns(const std::vector<std::string>& columnNames); // convenience overload
    void setData(const std::vector<std::vector<std::string>>& tableData);
    void setData(std::vector<std::vector<std::string>>&& tableData);
    void setCellEditedStatus(const std::vector<std::vector<bool>>& editedCells);
    void setRowsPendingDelete(const std::vector<bool>& rows);
    void setSelectedCell(int row, int col);
    [[nodiscard]] std::vector<int> getSelectedRows() const;
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
    void setCellColorCallback(CellColorCallback callback) {
        cellColorCb = callback;
    }
    void setCellEditableCallback(CellEditableCallback callback) {
        cellEditableCb = callback;
    }
    void setColumnNullableCallback(ColumnNullableCallback callback) {
        columnNullableCb = callback;
    }
    void setOnSetNull(OnSetNullCallback callback) {
        onSetNull = callback;
    }
    void setOnFilterByValue(OnFilterByValueCallback callback) {
        onFilterByValue = callback;
    }
    void setOnDeleteRow(OnDeleteRowCallback callback) {
        onDeleteRow = callback;
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
    std::vector<Column> columns;
    std::vector<std::vector<std::string>> data;
    std::vector<std::vector<bool>> editedCells;
    std::vector<bool> rowsPendingDelete;

    int selectedRow = -1;
    int selectedCol = -1;
    int editingRow = -1;
    int editingCol = -1;
    int rowNumberOffset = 0;

    // drag-to-select range — anchor is the mouse-down cell,
    // rangeEnd tracks the cell under the cursor while dragging.
    // When not dragging, the range spans [anchor..selectedRow/Col].
    int rangeAnchorRow = -1;
    int rangeAnchorCol = -1;
    int rangeEndRow = -1;
    int rangeEndCol = -1;
    bool isDragging = false;
    bool mouseWasDownLastFrame = false;
    // updated at the top of render(): true only on the frame the left button
    // transitions from up to down. more reliable than ImGui::IsMouseClicked
    // in scrollable tables where a held Selectable can interfere with input routing.
    bool leftPressedThisFrame = false;

    char editBuffer[16384] = {0};

    // Scrolling state
    bool shouldScrollToCell = false;
    int scrollTargetRow = -1;
    int scrollTargetCol = -1;

    // Edit mode state
    bool justEnteredEditWithChar = false;
    int initialCursorPos = 0;
    bool comboNeedsOpen = false;
    bool comboHasOpened = false;

    // edit overlay state
    bool pendingEditOverlay = false;
    int editOverlayFrames = 0;
    float editOverlayPosX = 0;
    float editOverlayPosY = 0;
    float editOverlayWidth = 0;

    // date/datetime picker state
    DateTimePickerState dtPickerState;
    bool datePickerNeedsOpen = false;
    bool datePickerDirty = false;

    // Callbacks
    OnCellEditCallback onCellEdit;
    OnCellSelectCallback onCellSelect;
    OnCellDoubleClickCallback onCellDoubleClick;
    OnSortChangedCallback onSortChanged;
    CellColorCallback cellColorCb;
    CellEditableCallback cellEditableCb;
    ColumnNullableCallback columnNullableCb;
    OnSetNullCallback onSetNull;
    OnFilterByValueCallback onFilterByValue;
    OnDeleteRowCallback onDeleteRow;

    // Sorting state
    int sortColumn = -1;
    SortDirection sortDirection = SortDirection::None;

    void renderCell(int row, int col);
    void handleCellInteraction(int row, int col, bool isSelected);
    void renderCellContextMenu(int row, int col);
    void copyCellToClipboard(int row, int col) const;
    void copyRowToClipboard(int row) const;
    void renderColumnHeader(int colIdx, const std::string& colName);
    void renderEditOverlay();
    void updateDragFromItem(int row, int col);
    void handleKeyboardNavigation();
    bool isInSelectionRange(int row, int col) const;
    void collapseSelectionToCell(int row, int col);
    void setSelectionRange(int anchorRow, int anchorCol, int endRow, int endCol);

    bool suppressBoolToggleUntilMouseUp = false;
    int suppressBoolToggleRow = -1;
    int suppressBoolToggleCol = -1;
};
