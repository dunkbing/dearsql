#pragma once

#include "TextEditor.h"
#include "ui/auto_complete_input.hpp"
#include "ui/table_renderer.hpp"
#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Forward declarations
class DatabaseInterface;

enum class TabType { SQL_EDITOR, TABLE_VIEWER };

class Tab {
public:
    Tab(std::string name, TabType type);
    virtual ~Tab() = default;

    // Common properties
    [[nodiscard]] const std::string& getName() const {
        return name;
    }
    void setName(const std::string& newName) {
        name = newName;
    }
    [[nodiscard]] TabType getType() const {
        return type;
    }
    [[nodiscard]] bool isOpen() const {
        return open;
    }
    void setOpen(const bool isOpen) {
        open = isOpen;
    }
    [[nodiscard]] bool shouldFocus() const {
        return needsFocus;
    }
    void setShouldFocus(const bool focus) {
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
    explicit SQLEditorTab(const std::string& name,
                          const std::shared_ptr<DatabaseInterface>& serverDatabase = nullptr,
                          const std::string& selectedDatabaseName = "");
    ~SQLEditorTab() override;

    void render() override;

    // SQL Editor specific methods
    [[nodiscard]] const std::string& getQuery() const {
        return sqlQuery;
    }
    void setQuery(const std::string& query) {
        sqlQuery = query;
    }
    [[nodiscard]] const std::string& getResult() const {
        return queryResult;
    }
    void setResult(const std::string& result) {
        queryResult = result;
    }
    [[nodiscard]] std::shared_ptr<DatabaseInterface> getServerDatabase() const {
        return serverDatabase;
    }
    void setServerDatabase(std::shared_ptr<DatabaseInterface> db) {
        serverDatabase = std::move(db);
    }
    [[nodiscard]] const std::string& getSelectedDatabaseName() const {
        return selectedDatabaseName;
    }
    void setSelectedDatabaseName(const std::string& dbName) {
        selectedDatabaseName = dbName;
    }
    [[nodiscard]] const std::string& getSelectedSchemaName() const {
        return selectedSchemaName;
    }
    void setSelectedSchemaName(const std::string& schemaName) {
        selectedSchemaName = schemaName;
    }

private:
    std::string sqlQuery;
    std::string queryResult;
    std::shared_ptr<DatabaseInterface> serverDatabase; // Server connection (Postgres/MySQL)
    std::string selectedDatabaseName;                  // Selected database within the server
    std::string selectedSchemaName;                    // Selected schema within the database
    TextEditor sqlEditor;

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
    void startQueryExecutionAsync(const std::shared_ptr<DatabaseInterface>& targetDb,
                                  const std::string& query);
    void checkQueryExecutionStatus();
    void cancelQueryExecution();
    void populateAutoCompleteKeywords();

    // Helper method for splitter
    bool renderVerticalSplitter(const char* id, float* position, float minSize1,
                                float minSize2) const;
};

class TableViewerTab final : public Tab {
public:
    TableViewerTab(const std::string& name, std::string databasePath, std::string tableName,
                   std::shared_ptr<DatabaseInterface> serverDatabase = nullptr);

    void render() override;

    // Table Viewer specific methods
    [[nodiscard]] const std::string& getDatabasePath() const {
        return databasePath;
    }
    [[nodiscard]] const std::string& getTableName() const {
        return tableName;
    }
    [[nodiscard]] std::shared_ptr<DatabaseInterface> getServerDatabase() const {
        return serverDatabase;
    }
    void setServerDatabase(std::shared_ptr<DatabaseInterface> db) {
        serverDatabase = std::move(db);
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
    [[nodiscard]] std::vector<std::string> getPrimaryKeyColumns() const;
    void showSaveConfirmationDialog();
    void checkSQLExecutionStatus();

private:
    std::string databasePath;
    std::string tableName;
    std::shared_ptr<DatabaseInterface> serverDatabase;
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

    // Filter functionality
    char filterBuffer[512] = {0};
    std::string currentFilter;
    bool filterChanged = false;
    std::unique_ptr<AutoCompleteInput> filterAutoComplete;

    // Helper methods
    void selectCell(int row, int col);
    void handleKeyboardNavigation();
    void applyFilter();
    void initializeFilterAutoComplete();
};
