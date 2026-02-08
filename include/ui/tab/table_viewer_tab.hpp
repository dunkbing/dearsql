#pragma once

#include "ui/auto_complete_input.hpp"
#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include <future>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
class IDatabaseNode;

class TableViewerTab final : public Tab {
public:
    TableViewerTab(const std::string& name, std::string databasePath, std::string tableName,
                   IDatabaseNode* node);

    void render() override;

    // Table Viewer specific methods
    [[nodiscard]] const std::string& getDatabasePath() const {
        return databasePath;
    }
    [[nodiscard]] const std::string& getTableName() const {
        return tableName;
    }
    [[nodiscard]] IDatabaseNode* getDatabaseNode() const {
        return node_;
    }
    void loadDataAsync();
    void checkAsyncLoadStatus();
    void nextPage();
    void previousPage();
    void firstPage();
    void lastPage();
    void refreshData();
    void saveChanges();
    void cancelChanges();

    // SQL generation and confirmation dialog
    std::vector<std::string> generateUpdateSQL();
    [[nodiscard]] std::vector<std::string> getPrimaryKeyColumns() const;
    void showSaveConfirmationDialog();
    void checkSQLExecutionStatus();

private:
    std::string databasePath;
    std::string tableName;
    IDatabaseNode* node_ = nullptr;
    std::vector<std::vector<std::string>> tableData;
    std::vector<std::vector<std::string>> originalData;
    std::vector<std::string> columnNames;
    std::vector<std::vector<bool>> editedCells;
    int currentPage = 0;
    int rowsPerPage = 100;
    int totalRows = 0;

    // Async loading state
    bool isLoadingData = false;
    bool hasLoadingError = false;
    std::string loadingError;
    std::future<void> dataLoadFuture;

    // Edit state
    int selectedRow = -1;
    int selectedCol = -1;
    bool hasChanges = false;

    // Save confirmation dialog state
    bool showSaveDialog = false;
    bool dialogOpened = false;
    std::vector<std::string> pendingUpdateSQL;

    // Async SQL execution state
    bool executingSQL = false;
    std::future<std::pair<bool, std::string>> sqlExecutionFuture;

    // Table renderer
    std::unique_ptr<TableRenderer> tableRenderer;

    // Filter functionality
    char filterBuffer[512] = {0};
    std::string currentFilter;
    bool filterChanged = false;
    std::unique_ptr<AutoCompleteInput> filterAutoComplete;

    // Sorting state
    int sortColumn = -1;
    std::string sortColumnName;
    SortDirection sortDirection = SortDirection::None;

    // Right panel state
    bool rightPanelOpen = false;
    float rightPanelWidth = 300.0f;
    int activeRightPanelTab = 0; // 0 = Value, 1 = Metadata
    char valuePanelBuffer[4096] = {0};
    bool valuePanelBufferDirty = false;
    int lastSyncedRow = -1;
    int lastSyncedCol = -1;
    std::string metadataFilter;

    // Helper methods
    void initializeTableRenderer();
    void selectCell(int row, int col);
    void handleKeyboardNavigation();
    void applyFilter();
    void initializeFilterAutoComplete();

    // Right panel methods
    void renderRightPanelToggleStrip(float stripWidth, float availableHeight);
    void renderRightPanel(float panelWidth, float availableHeight);
    void renderValueTab();
    void renderMetadataTab();
    void syncValuePanelBuffer();
};
