#pragma once

#include "TextEditor.h"
#include "ui/table_renderer.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <vector>

// Forward declarations
class DatabaseInterface;

enum class TabType { SQL_EDITOR, TABLE_VIEWER };

class Tab {
public:
    Tab(std::string name, TabType type);
    virtual ~Tab() = default;

    // Common properties
    [[nodiscard]] const std::string &getName() const {
        return name;
    }
    void setName(const std::string &newName) {
        name = newName;
    }
    [[nodiscard]] TabType getType() const {
        return type;
    }
    [[nodiscard]] bool isOpen() const {
        return open;
    }
    void setOpen(bool isOpen) {
        open = isOpen;
    }
    [[nodiscard]] bool shouldFocus() const {
        return needsFocus;
    }
    void setShouldFocus(bool focus) {
        needsFocus = focus;
    }

    // Virtual method for rendering tab content
    virtual void render() = 0;

protected:
    std::string name;
    TabType type;
    bool open = true;
    bool needsFocus = false;
};

class SQLEditorTab final : public Tab {
public:
    explicit SQLEditorTab(const std::string &name,
                          const std::string &databaseConnectionString = "");
    ~SQLEditorTab();

    void render() override;

    // SQL Editor specific methods
    [[nodiscard]] const std::string &getQuery() const {
        return sqlQuery;
    }
    void setQuery(const std::string &query) {
        sqlQuery = query;
    }
    [[nodiscard]] const std::string &getResult() const {
        return queryResult;
    }
    void setResult(const std::string &result) {
        queryResult = result;
    }
    [[nodiscard]] const std::string &getDatabaseConnectionString() const {
        return databaseConnectionString;
    }

private:
    std::string sqlQuery;
    std::string queryResult;
    std::string databaseConnectionString;
    TextEditor sqlEditor;
    char resultBuffer[16384] = "";

    // Structured query results for table display
    std::vector<std::string> queryColumnNames;
    std::vector<std::vector<std::string>> queryTableData;
    bool hasStructuredResults = false;
    std::string queryError;
    std::chrono::milliseconds lastQueryDuration{0};

    // Async query execution state
    bool isExecutingQuery = false;
    std::future<void> queryExecutionFuture;
    std::atomic<bool> shouldCancelQuery{false};

    // Splitter state for resizing between editor and results
    float splitterPosition = 0.4f; // 40% for editor, 60% for results
    bool splitterActive = false;
    float totalContentHeight = 0.0f; // Store total height for consistent splitter calculation

    // Helper methods for async execution
    void startQueryExecutionAsync(const std::shared_ptr<DatabaseInterface> &targetDb,
                                  const std::string &query);
    void checkQueryExecutionStatus();
    void cancelQueryExecution();

    // Helper method for splitter
    bool renderVerticalSplitter(const char *id, float *position, float minSize1,
                                float minSize2) const;
};

class TableViewerTab final : public Tab {
public:
    TableViewerTab(const std::string &name, std::string databasePath, std::string tableName);

    void render() override;

    // Table Viewer specific methods
    [[nodiscard]] const std::string &getDatabasePath() const {
        return databasePath;
    }
    [[nodiscard]] const std::string &getTableName() const {
        return tableName;
    }
    void loadData();
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
    std::vector<std::string> getPrimaryKeyColumns() const;
    void showSaveConfirmationDialog();
    void checkSQLExecutionStatus();

private:
    std::string databasePath;
    std::string tableName;
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

    // Helper methods
    void selectCell(int row, int col);
};
