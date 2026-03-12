#include "database/bigquery.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <fstream>
#include <ranges>

#include <google/cloud/bigquery/v2/dataset.pb.h>
#include <google/cloud/bigquery/v2/job.pb.h>
#include <google/cloud/common_options.h>
#include <google/cloud/credentials.h>
#include <google/protobuf/struct.pb.h>

namespace gc = google::cloud;
namespace bqcontrol = gc::bigquerycontrol_v2;

BigQueryDatabase::BigQueryDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
    // project ID is stored in the "database" field
    projectId = connectionInfo.database;
    Logger::debug(std::format("Creating BigQueryDatabase for project '{}'", projectId));
}

BigQueryDatabase::~BigQueryDatabase() {
    datasetsLoader.cancel();
    refreshWorkflow.cancel();

    for (auto& ptr : datasetCache | std::views::values) {
        if (ptr) {
            ptr->tablesLoader.cancel();
            ptr->viewsLoader.cancel();
        }
    }

    disconnect();
}

BigQueryDatasetNode* BigQueryDatabase::getDatabaseData(const std::string& datasetId) {
    auto it = datasetCache.find(datasetId);
    if (it == datasetCache.end()) {
        auto node = std::make_unique<BigQueryDatasetNode>();
        node->name = datasetId;
        node->parentDb = this;
        auto* ptr = node.get();
        datasetCache[datasetId] = std::move(node);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> BigQueryDatabase::connect() {
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);

    if (projectId.empty()) {
        std::string err = "BigQuery: project ID is required (set in Database field)";
        setLastConnectionError(err);
        return {false, err};
    }

    try {
        // set credentials from the service account key file
        gc::Options opts;
        if (!connectionInfo.host.empty()) {
            // custom endpoint (e.g. bigquery-emulator)
            auto endpoint = connectionInfo.host;
            if (connectionInfo.port > 0) {
                endpoint += ":" + std::to_string(connectionInfo.port);
            }
            if (endpoint.find("://") == std::string::npos) {
                endpoint = "http://" + endpoint;
            }
            opts.set<gc::EndpointOption>(endpoint);
            opts.set<gc::UnifiedCredentialsOption>(gc::MakeInsecureCredentials());
        } else if (!connectionInfo.path.empty()) {
            std::ifstream keyFile(connectionInfo.path);
            if (!keyFile.is_open()) {
                std::string err =
                    std::format("Cannot open service account key file: {}", connectionInfo.path);
                setLastConnectionError(err);
                return {false, err};
            }
            std::string jsonKey(std::istreambuf_iterator<char>(keyFile), {});
            opts.set<gc::UnifiedCredentialsOption>(
                gc::MakeServiceAccountCredentials(std::move(jsonKey)));
        }
        // else: uses Application Default Credentials (ADC)

        jobClient = std::make_shared<bqcontrol::JobServiceClient>(
            bqcontrol::MakeJobServiceConnectionRest(opts));
        datasetClient = std::make_shared<bqcontrol::DatasetServiceClient>(
            bqcontrol::MakeDatasetServiceConnectionRest(opts));
        tableClient = std::make_shared<bqcontrol::TableServiceClient>(
            bqcontrol::MakeTableServiceConnectionRest(opts));

        // verify connectivity by listing datasets (limit to 1)
        google::cloud::bigquery::v2::ListDatasetsRequest listReq;
        listReq.set_project_id(projectId);
        listReq.mutable_max_results()->set_value(1);

        auto result = datasetClient->ListDatasets(listReq);
        for (auto& ds : result) {
            if (!ds.ok()) {
                std::string err =
                    std::format("BigQuery connection failed: {}", ds.status().message());
                setLastConnectionError(err);
                connected = false;
                return {false, err};
            }
            break; // just need to verify one works
        }

        Logger::info(std::format("Connected to BigQuery project: {}", projectId));
        connected = true;
        setLastConnectionError("");

        if (!datasetsLoaded && !datasetsLoader.isRunning()) {
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const std::exception& e) {
        std::string err = std::format("BigQuery connection failed: {}", e.what());
        Logger::error(err);
        setLastConnectionError(err);
        connected = false;
        return {false, err};
    }
}

void BigQueryDatabase::disconnect() {
    std::lock_guard lock(clientMutex);
    jobClient.reset();
    datasetClient.reset();
    tableClient.reset();
    connected = false;
}

void BigQueryDatabase::refreshConnection() {
    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        auto [ok, err] = connect();
        if (!ok) {
            setLastConnectionError(err);
            return false;
        }

        auto datasets = getDatasetsAsync();
        {
            std::lock_guard lock(refreshStateMutex);
            pendingRefreshDatasets = std::move(datasets);
        }

        Logger::info(
            std::format("BigQuery refresh completed for {} datasets", datasetCache.size()));
        return true;
    });
}

QueryResult BigQueryDatabase::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connected || !jobClient) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Not connected to BigQuery";
        result.statements.push_back(r);
        return result;
    }

    try {
        google::cloud::bigquery::v2::PostQueryRequest req;
        req.set_project_id(projectId);
        auto* queryReq = req.mutable_query_request();
        queryReq->set_query(query);
        queryReq->mutable_use_legacy_sql()->set_value(false);
        queryReq->mutable_max_results()->set_value(static_cast<uint32_t>(rowLimit));

        auto response = jobClient->Query(req);
        if (!response.ok()) {
            StatementResult r;
            r.success = false;
            r.errorMessage = std::format("BigQuery query failed: {}", response.status().message());
            result.statements.push_back(r);
        } else {
            StatementResult r;
            const auto& resp = *response;

            // extract column names from schema
            if (resp.schema().fields_size() > 0) {
                for (const auto& field : resp.schema().fields()) {
                    r.columnNames.push_back(field.name());
                }
            }

            // extract row data — rows are google::protobuf::Struct
            // BigQuery REST format: each row has "f" (list of cells), each cell has "v" (value)
            for (const auto& row : resp.rows()) {
                std::vector<std::string> rowData;
                auto fIt = row.fields().find("f");
                if (fIt == row.fields().end())
                    continue;
                for (const auto& cell : fIt->second.list_value().values()) {
                    const auto& cellStruct = cell.struct_value();
                    auto vIt = cellStruct.fields().find("v");
                    if (vIt != cellStruct.fields().end()) {
                        const auto& val = vIt->second;
                        if (val.kind_case() == google::protobuf::Value::kNullValue) {
                            rowData.push_back("NULL");
                        } else {
                            rowData.push_back(val.string_value());
                        }
                    } else {
                        rowData.push_back("NULL");
                    }
                }
                r.tableData.push_back(std::move(rowData));
            }

            if (!r.columnNames.empty()) {
                r.message = std::format("Returned {} row{}", r.tableData.size(),
                                        r.tableData.size() == 1 ? "" : "s");
                if (resp.total_rows().value() > static_cast<uint64_t>(rowLimit)) {
                    r.message += std::format(" (limited to {})", rowLimit);
                }
            } else {
                r.message = "Query executed successfully";
            }

            result.statements.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        StatementResult r;
        r.success = false;
        r.errorMessage = e.what();
        result.statements.push_back(r);
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

std::unordered_map<std::string, std::unique_ptr<BigQueryDatasetNode>>&
BigQueryDatabase::getDatabaseDataMap() {
    if (!datasetsLoaded && !datasetsLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return datasetCache;
}

void BigQueryDatabase::refreshDatabaseNames() {
    if (datasetsLoader.isRunning())
        return;

    datasetsLoaded = false;
    datasetsLoader.start([this]() { return getDatasetsAsync(); });
}

bool BigQueryDatabase::isLoadingDatabases() const {
    return datasetsLoader.isRunning();
}

bool BigQueryDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases())
        return true;

    for (const auto& [_, node] : datasetCache) {
        if (!node)
            continue;
        if (node->tablesLoader.isRunning() || node->viewsLoader.isRunning())
            return true;
    }
    return false;
}

void BigQueryDatabase::checkDatabasesStatusAsync() {
    datasetsLoader.check([this](const std::vector<std::string>& datasets) {
        Logger::info(std::format("BigQuery: loaded {} datasets", datasets.size()));

        for (const auto& ds : datasets) {
            getDatabaseData(ds);
        }

        datasetsLoaded = true;
    });
}

void BigQueryDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([this](const bool success) {
        if (success) {
            Logger::info("BigQuery refresh workflow completed");
            std::vector<std::string> refreshed;
            {
                std::lock_guard lock(refreshStateMutex);
                refreshed = std::move(pendingRefreshDatasets);
                pendingRefreshDatasets.clear();
            }

            for (const auto& ds : refreshed) {
                getDatabaseData(ds);
            }

            datasetsLoaded = true;

            for (auto& [_, node] : datasetCache) {
                if (node) {
                    node->startTablesLoadAsync(true);
                    node->startViewsLoadAsync(true);
                }
            }
        } else {
            Logger::error("BigQuery refresh workflow failed");
        }
    });
}

std::vector<std::string> BigQueryDatabase::getDatasetsAsync() const {
    Logger::info("getDatasetsAsync (BigQuery)");
    std::vector<std::string> result;

    try {
        if (!isConnected() || !datasetClient) {
            Logger::error("Cannot load datasets: not connected");
            return result;
        }

        google::cloud::bigquery::v2::ListDatasetsRequest req;
        req.set_project_id(projectId);

        auto datasets = datasetClient->ListDatasets(req);
        for (auto& ds : datasets) {
            if (!ds.ok()) {
                Logger::error(std::format("Error listing datasets: {}", ds.status().message()));
                break;
            }
            if (!ds->dataset_reference().dataset_id().empty()) {
                result.push_back(ds->dataset_reference().dataset_id());
            }
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to list BigQuery datasets: {}", e.what()));
    }

    Logger::info(std::format("Found {} datasets", result.size()));
    return result;
}

std::pair<bool, std::string> BigQueryDatabase::createDatabase(const std::string& dbName,
                                                              const std::string& comment) {
    if (!connected || !datasetClient) {
        return {false, "Not connected to BigQuery"};
    }

    try {
        google::cloud::bigquery::v2::InsertDatasetRequest req;
        req.set_project_id(projectId);
        auto* dataset = req.mutable_dataset();
        auto* ref = dataset->mutable_dataset_reference();
        ref->set_project_id(projectId);
        ref->set_dataset_id(dbName);
        if (!comment.empty()) {
            dataset->mutable_description()->set_value(comment);
        }

        auto result = datasetClient->InsertDataset(req);
        if (!result.ok()) {
            return {false, std::format("Failed to create dataset: {}", result.status().message())};
        }

        Logger::info(std::format("Dataset '{}' created", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

std::pair<bool, std::string> BigQueryDatabase::dropDatabase(const std::string& dbName) {
    if (!connected || !datasetClient) {
        return {false, "Not connected to BigQuery"};
    }

    try {
        google::cloud::bigquery::v2::DeleteDatasetRequest req;
        req.set_project_id(projectId);
        req.set_dataset_id(dbName);
        req.set_delete_contents(true);

        auto result = datasetClient->DeleteDataset(req);
        if (!result.ok()) {
            return {false, std::format("Failed to delete dataset: {}", result.message())};
        }

        datasetCache.erase(dbName);
        Logger::info(std::format("Dataset '{}' deleted", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}
