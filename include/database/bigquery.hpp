#pragma once

#include "async_helper.hpp"
#include "bigquery/bigquery_dataset_node.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include <mutex>
#include <unordered_map>
#include <vector>

#include <google/cloud/bigquerycontrol/v2/dataset_client.h>
#include <google/cloud/bigquerycontrol/v2/job_client.h>
#include <google/cloud/bigquerycontrol/v2/table_client.h>

class BigQueryDatabase final : public DatabaseInterface, public IQueryExecutor {
    friend class BigQueryDatasetNode;

public:
    BigQueryDatabase(const DatabaseConnectionInfo& connInfo);
    ~BigQueryDatabase() override;

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
        return datasetsLoaded;
    }
    bool isLoadingDatabases() const;
    void checkDatabasesStatusAsync();
    void checkRefreshWorkflowAsync();

    [[nodiscard]] bool hasPendingAsyncWork() const override;

    BigQueryDatasetNode* getDatabaseData(const std::string& datasetId);

    std::unordered_map<std::string, std::unique_ptr<BigQueryDatasetNode>>& getDatabaseDataMap();
    const std::unordered_map<std::string, std::unique_ptr<BigQueryDatasetNode>>&
    getDatabaseDataMap() const {
        return datasetCache;
    }

    // accessors for child nodes
    const std::string& getProjectId() const {
        return projectId;
    }

    google::cloud::bigquerycontrol_v2::JobServiceClient& getJobClient() {
        return *jobClient;
    }
    google::cloud::bigquerycontrol_v2::TableServiceClient& getTableClient() {
        return *tableClient;
    }

protected:
    std::vector<std::string> getDatasetsAsync() const;

private:
    std::string projectId;

    std::shared_ptr<google::cloud::bigquerycontrol_v2::JobServiceClient> jobClient;
    std::shared_ptr<google::cloud::bigquerycontrol_v2::DatasetServiceClient> datasetClient;
    std::shared_ptr<google::cloud::bigquerycontrol_v2::TableServiceClient> tableClient;

    std::unordered_map<std::string, std::unique_ptr<BigQueryDatasetNode>> datasetCache;
    bool datasetsLoaded = false;
    std::vector<std::string> pendingRefreshDatasets;
    mutable std::mutex refreshStateMutex;
    AsyncOperation<std::vector<std::string>> datasetsLoader;
    AsyncOperation<bool> refreshWorkflow;
    mutable std::mutex clientMutex;
};
