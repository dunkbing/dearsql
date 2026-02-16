#pragma once

#include "database/async_helper.hpp"
#include "database/database_node.hpp"
#include "database/db.hpp"
#include "database/db_interface.hpp"
#include "database/table_data_provider.hpp"
#include <map>
#include <string>
#include <vector>

// Forward declaration
class MongoDBDatabase;

/**
 * @brief Per-database data for MongoDB
 *
 * MongoDB hierarchy: Server -> Databases -> Collections
 * Each MongoDBDatabaseNode represents one database within the MongoDB server.
 * Collections are mapped to the "tables" concept for UI consistency.
 */
class MongoDBDatabaseNode : public IDatabaseNode, public ITableDataProvider {
public:
    MongoDBDatabase* parentDb = nullptr;

    std::string name;

    // MongoDB: Database -> Collections (mapped to tables for consistency)
    std::vector<Table> collections; // Collections treated as "tables"
    std::vector<Table> views;       // MongoDB views (if any)

    // Loading state flags
    bool collectionsLoaded = false;
    bool viewsLoaded = false;

    // Async operations
    AsyncOperation<std::vector<Table>> collectionsLoader;
    AsyncOperation<std::vector<Table>> viewsLoader;
    std::map<std::string, AsyncOperation<Table>> collectionRefreshLoaders;

    // Error tracking
    std::string lastCollectionsError;
    std::string lastViewsError;

    // UI expansion state
    bool expanded = false;
    bool collectionsExpanded = false;
    bool viewsExpanded = false;

    // ========== IDatabaseNode Implementation ==========

    [[nodiscard]] std::string getName() const override {
        return name;
    }

    [[nodiscard]] std::string getFullPath() const override;

    [[nodiscard]] DatabaseType getDatabaseType() const override {
        return DatabaseType::MONGODB;
    }

    std::vector<QueryResult> executeQuery(const std::string& sql, int limit = 1000) override;

    std::vector<Table>& getTables() override {
        return collections;
    }
    [[nodiscard]] const std::vector<Table>& getTables() const override {
        return collections;
    }

    std::vector<Table>& getViews() override {
        return views;
    }
    [[nodiscard]] const std::vector<Table>& getViews() const override {
        return views;
    }

    std::vector<std::vector<std::string>> getTableData(const std::string& collectionName, int limit,
                                                       int offset, const std::string& filter = "",
                                                       const std::string& sort = "") override;
    std::vector<std::string> getColumnNames(const std::string& collectionName) override;
    int getRowCount(const std::string& collectionName, const std::string& filter = "") override;

    [[nodiscard]] bool isTablesLoaded() const override {
        return collectionsLoaded;
    }
    [[nodiscard]] bool isViewsLoaded() const override {
        return viewsLoaded;
    }
    [[nodiscard]] bool isLoadingTables() const override {
        return collectionsLoader.isRunning();
    }
    [[nodiscard]] bool isLoadingViews() const override {
        return viewsLoader.isRunning();
    }

    void startTablesLoadAsync(bool force = false) override;
    void startViewsLoadAsync(bool force = false) override;
    void checkLoadingStatus() override;

    [[nodiscard]] const std::string& getLastTablesError() const override {
        return lastCollectionsError;
    }
    [[nodiscard]] const std::string& getLastViewsError() const override {
        return lastViewsError;
    }

    void startTableRefreshAsync(const std::string& collectionName) override;
    [[nodiscard]] bool isTableRefreshing(const std::string& collectionName) const override;
    void checkTableRefreshStatusAsync(const std::string& collectionName) override;

    // ========== MongoDB-specific Methods ==========

    void startCollectionsLoadAsync(bool force = false);
    void checkCollectionsStatusAsync();
    std::vector<Table> getCollectionsAsync();

    Table refreshCollectionAsync(const std::string& collectionName);

    // Schema inference by sampling documents
    std::vector<Column> inferSchemaFromSample(const std::string& collectionName,
                                              int sampleSize = 100);
};
