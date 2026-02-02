#pragma once

#include "async_helper.hpp"
#include "db_interface.hpp"
#include "mongodb/mongodb_database_node.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <unordered_map>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

class MongoDBDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class MongoDBDatabaseNode;

public:
    MongoDBDatabase(const DatabaseConnectionInfo& connInfo);
    ~MongoDBDatabase() override;

    // Connection management
    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    // Database operations
    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    // IQueryExecutor implementation (for JSON/BSON commands)
    QueryResult executeQueryWithResult(const std::string& query, int rowLimit = 1000) override;
    std::pair<bool, std::string> executeQuery(const std::string& query) override;

    // Database list methods
    void refreshDatabaseNames();

    // Connection status
    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow.isRunning();
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

    // Async operation status
    [[nodiscard]] bool hasPendingAsyncWork() const override;

protected:
    // Async loading helpers
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    // MongoDB driver instance (must be initialized once per application)
    static mongocxx::instance& getDriverInstance();

    // Connection pool
    std::unique_ptr<mongocxx::pool> connectionPool;

    // Database cache
    std::unordered_map<std::string, std::unique_ptr<MongoDBDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;

    // Async database loading
    AsyncOperation<std::vector<std::string>> databasesLoader;

    // Async refresh workflow
    AsyncOperation<bool> refreshWorkflow;

public:
    // Helper methods for per-database data access
    MongoDBDatabaseNode* getDatabaseData(const std::string& dbName);

    // Accessor for database data map
    std::unordered_map<std::string, std::unique_ptr<MongoDBDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<MongoDBDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

    // Get a client from the pool
    mongocxx::pool::entry getClient() const;

private:
    // Thread synchronization
    mutable std::mutex poolMutex;
};
