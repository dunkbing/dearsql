#pragma once

#include "async_helper.hpp"
#include "connection_pool.hpp"
#include "db_interface.hpp"
#include "oracle/oracle_database_node.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

#include "oracle/oracle_fwd.hpp"

class OracleDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class OracleDatabaseNode;

public:
    OracleDatabase(const DatabaseConnectionInfo& connInfo);
    ~OracleDatabase() override;

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

    OracleDatabaseNode* getDatabaseData(const std::string& schemaName);

    std::unordered_map<std::string, std::unique_ptr<OracleDatabaseNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<OracleDatabaseNode>>&
    getDatabaseDataMap() const {
        return databaseDataCache;
    }

    // ODPI-C context (shared across all Oracle connections)
    static dpiContext* getContext();

    // returns true if Oracle Client library is not available (needs install)
    static bool needsClientInstall();

    // re-attempt context initialization (call after Oracle Client is installed)
    static void reinitContext();

protected:
    std::vector<std::string> getDatabaseNamesAsync() const;

private:
    std::unordered_map<std::string, std::unique_ptr<OracleDatabaseNode>> databaseDataCache;
    bool databasesLoaded = false;
    std::vector<std::string> pendingRefreshDatabaseNames;
    mutable std::mutex refreshStateMutex;
    AsyncOperation<std::vector<std::string>> databasesLoader;
    AsyncOperation<bool> refreshWorkflow;
    mutable std::mutex sessionMutex;

    static void initContext();
    void ensureConnectionPoolForSchema(const DatabaseConnectionInfo& info);
    ConnectionPool<dpiConn*>::Session getSession() const;
};
