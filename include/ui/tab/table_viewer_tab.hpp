#pragma once

#include "ui/auto_complete_input.hpp"
#include "ui/tab/tab.hpp"
#include "ui/table_renderer.hpp"
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// Forward declarations
class PostgresSchemaNode;
class MySQLDatabaseNode;

class TableViewerTab final : public Tab {
public:
    using DatabaseNode = std::variant<PostgresSchemaNode*, MySQLDatabaseNode*>;

    TableViewerTab(const std::string& name, std::string databasePath, std::string tableName,
                   PostgresSchemaNode* schemaNode);
    TableViewerTab(const std::string& name, std::string databasePath, std::string tableName,
                   MySQLDatabaseNode* mysqlNode);

    void render() override;

    // Table Viewer specific methods
    [[nodiscard]] const std::string& getDatabasePath() const {
        return databasePath;
    }
    [[nodiscard]] const std::string& getTableName() const {
        return tableName;
    }
    [[nodiscard]] const DatabaseNode& getDatabaseNode() const {
        return databaseNode;
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
    DatabaseNode databaseNode;
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
