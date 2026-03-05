#pragma once

#include "async_helper.hpp"
#include "connection_pool.hpp"
#include "db_interface.hpp"
#include "mssql/mssql_database_node.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

#include <sybdb.h>

class MSSQLDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class MSSQLDatabaseNode;

public:
    MSSQLDatabase(const DatabaseConnectionInfo& connInfo);
    ~MSSQLDatabase() override;

    std::pair<bool, std::string> connect() override;
    void disconnect() override;
    void refreshConnection() override;

    std::pair<bool, std::string> createDatabase(const std::string& dbName,
                                                const std::string& comment = "") override;
    std::pair<bool, std::string> dropDatabase(const std::string& dbName) override;

    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override;

    void refreshDatabaseNames();

    bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow.isRunning();
    }

    bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

    [[nodiscard]] bool hasPendingAsyncWork() const override;

    MSSQLDatabaseNode* getDatabaseData(const std::string& dbName);

    std::unordered_map<std::string, std::unique_ptr<MSSQLDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<MSSQLDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

protected:
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    std::unordered_map<std::string, std::unique_ptr<MSSQLDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;
    std::vector<std::string> pendingRefreshDatabaseNames;
    mutable std::mutex refreshStateMutex;
    AsyncOperation<std::vector<std::string>> databasesLoader;
    AsyncOperation<bool> refreshWorkflow;
    mutable std::mutex sessionMutex;

    static void initDbLib();
    void ensureConnectionPoolForDatabase(const DatabaseConnectionInfo& info);
    ConnectionPool<DBPROCESS*>::Session getSession() const;
};
