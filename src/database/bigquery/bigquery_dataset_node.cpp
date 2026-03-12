#include "database/bigquery/bigquery_dataset_node.hpp"
#include "database/bigquery.hpp"
#include "database/db.hpp"
#include "database/ddl_builder.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <ranges>

#include <google/cloud/bigquery/v2/table.pb.h>
#include <google/cloud/bigquery/v2/table_schema.pb.h>

namespace gc = google::cloud;

namespace {

    // map BigQuery type string to a simpler display type
    std::string bqTypeToDisplay(const std::string& type) {
        // BigQuery types: STRING, BYTES, INTEGER, INT64, FLOAT, FLOAT64,
        // NUMERIC, BIGNUMERIC, BOOLEAN, BOOL, TIMESTAMP, DATE, TIME, DATETIME,
        // GEOGRAPHY, RECORD, STRUCT, ARRAY, JSON, RANGE
        return type;
    }

    std::vector<Column> extractColumns(
        const google::protobuf::RepeatedPtrField<google::cloud::bigquery::v2::TableFieldSchema>&
            fields) {
        std::vector<Column> columns;
        for (const auto& field : fields) {
            Column col;
            col.name = field.name();
            col.type = bqTypeToDisplay(field.type());
            col.isNotNull = (field.mode() == "REQUIRED");
            if (field.has_description() && !field.description().value().empty()) {
                col.comment = field.description().value();
            }
            columns.push_back(std::move(col));
        }
        return columns;
    }

} // namespace

std::string BigQueryDatasetNode::getFullPath() const {
    if (parentDb) {
        return parentDb->getProjectId() + "." + name;
    }
    return name;
}

DatabaseType BigQueryDatasetNode::getDatabaseType() const {
    return DatabaseType::BIGQUERY;
}

QueryResult BigQueryDatasetNode::executeQuery(const std::string& query, int rowLimit) {
    if (!parentDb) {
        QueryResult r;
        StatementResult sr;
        sr.success = false;
        sr.errorMessage = "No parent database connection";
        r.statements.push_back(sr);
        return r;
    }
    // prefix unqualified table references with dataset
    // user is expected to write fully qualified names or use the default dataset
    return parentDb->executeQuery(query, rowLimit);
}

std::pair<bool, std::string> BigQueryDatasetNode::createTable(const Table& table) {
    try {
        DDLBuilder builder(getDatabaseType());
        std::string sql = builder.createTable(table, name);

        auto result = executeQuery(sql);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

void BigQueryDatasetNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        Logger::info(std::format("BigQuery: loaded {} tables for dataset {}", tables.size(), name));
        tablesLoaded = true;
    });
}

void BigQueryDatasetNode::startTablesLoadAsync(bool forceRefresh) {
    Logger::debug("startTablesLoadAsync for dataset: " + name + (forceRefresh ? " (force)" : ""));
    if (!parentDb || tablesLoader.isRunning())
        return;

    if (forceRefresh) {
        tablesLoaded = false;
        lastTablesError.clear();
    }

    if (!forceRefresh && tablesLoaded)
        return;

    tables.clear();
    tablesLoader.start([this]() { return getTablesAsync(); });
}

std::vector<Table> BigQueryDatasetNode::getTablesAsync() {
    std::vector<Table> result;

    if (!tablesLoader.isRunning() || !parentDb)
        return result;

    try {
        auto& tableClient = parentDb->getTableClient();
        const auto& project = parentDb->getProjectId();

        google::cloud::bigquery::v2::ListTablesRequest req;
        req.set_project_id(project);
        req.set_dataset_id(name);

        auto tables = tableClient.ListTables(req);

        struct TableInfo {
            std::string id;
            std::string type; // TABLE or VIEW
        };
        std::vector<TableInfo> tableInfos;

        for (auto& t : tables) {
            if (!tablesLoader.isRunning())
                return result;

            if (!t.ok()) {
                Logger::error(std::format("Error listing tables: {}", t.status().message()));
                lastTablesError = t.status().message();
                break;
            }

            std::string tableType = t->type();
            if (tableType == "TABLE" || tableType == "BASE TABLE") {
                tableInfos.push_back({t->table_reference().table_id(), tableType});
            }
        }

        Logger::debug(std::format("Found {} tables in dataset {}", tableInfos.size(), name));

        // load metadata for each table
        const auto connName = parentDb->getConnectionInfo().name;
        for (const auto& ti : tableInfos) {
            if (!tablesLoader.isRunning())
                break;

            google::cloud::bigquery::v2::GetTableRequest getReq;
            getReq.set_project_id(project);
            getReq.set_dataset_id(name);
            getReq.set_table_id(ti.id);

            auto tableResult = tableClient.GetTable(getReq);
            Table table;
            table.name = ti.id;
            table.fullName = connName + "." + name + "." + ti.id;

            if (tableResult.ok()) {
                table.columns = extractColumns(tableResult->schema().fields());
            } else {
                Logger::warn(std::format("Failed to get schema for {}.{}: {}", name, ti.id,
                                         tableResult.status().message()));
            }

            result.push_back(std::move(table));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting tables for dataset {}: {}", name, e.what()));
        lastTablesError = e.what();
    }

    return result;
}

void BigQueryDatasetNode::checkViewsStatusAsync() {
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        Logger::info(std::format("BigQuery: loaded {} views for dataset {}", views.size(), name));
        viewsLoaded = true;
    });
}

void BigQueryDatasetNode::startViewsLoadAsync(bool forceRefresh) {
    Logger::debug("startViewsLoadAsync for dataset: " + name);
    if (!parentDb || viewsLoader.isRunning())
        return;

    if (viewsLoader.isRunning() || (viewsLoaded && !forceRefresh))
        return;

    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    viewsLoader.start([this]() { return getViewsAsync(); });
}

std::vector<Table> BigQueryDatasetNode::getViewsAsync() {
    std::vector<Table> result;

    if (!viewsLoader.isRunning() || !parentDb)
        return result;

    try {
        auto& tableClient = parentDb->getTableClient();
        const auto& project = parentDb->getProjectId();

        google::cloud::bigquery::v2::ListTablesRequest req;
        req.set_project_id(project);
        req.set_dataset_id(name);

        auto tables = tableClient.ListTables(req);
        const auto connName = parentDb->getConnectionInfo().name;

        for (auto& t : tables) {
            if (!viewsLoader.isRunning())
                return result;

            if (!t.ok()) {
                Logger::error(std::format("Error listing views: {}", t.status().message()));
                lastViewsError = t.status().message();
                break;
            }

            if (t->type() != "VIEW")
                continue;

            google::cloud::bigquery::v2::GetTableRequest getReq;
            getReq.set_project_id(project);
            getReq.set_dataset_id(name);
            getReq.set_table_id(t->table_reference().table_id());

            Table view;
            view.name = t->table_reference().table_id();
            view.fullName = connName + "." + name + "." + view.name;

            auto tableResult = tableClient.GetTable(getReq);
            if (tableResult.ok()) {
                view.columns = extractColumns(tableResult->schema().fields());
            }

            result.push_back(std::move(view));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting views for dataset {}: {}", name, e.what()));
        lastViewsError = e.what();
    }

    return result;
}

void BigQueryDatasetNode::startTableRefreshAsync(const std::string& tableName) {
    Logger::debug(std::format("Refreshing table: {}", tableName));

    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning())
        return;

    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

void BigQueryDatasetNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end())
        return;

    it->second.check([this, tableName](const Table& refreshedTable) {
        auto tableIt = std::ranges::find_if(
            tables, [&tableName](const Table& t) { return t.name == tableName; });

        if (tableIt != tables.end()) {
            *tableIt = refreshedTable;
            Logger::info(std::format("Table {} refreshed", tableName));
        }

        tableRefreshLoaders.erase(tableName);
    });
}

Table BigQueryDatasetNode::refreshTableAsync(const std::string& tableName) {
    Table table;
    table.name = tableName;

    if (!parentDb)
        return table;

    try {
        auto& tableClient = parentDb->getTableClient();

        google::cloud::bigquery::v2::GetTableRequest req;
        req.set_project_id(parentDb->getProjectId());
        req.set_dataset_id(name);
        req.set_table_id(tableName);

        auto result = tableClient.GetTable(req);
        if (result.ok()) {
            table.columns = extractColumns(result->schema().fields());
            table.fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;
        } else {
            Logger::error(
                std::format("Error refreshing table {}: {}", tableName, result.status().message()));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error refreshing table {}: {}", tableName, e.what()));
        throw;
    }

    return table;
}

bool BigQueryDatasetNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

std::vector<std::vector<std::string>>
BigQueryDatasetNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                  const std::string& whereClause,
                                  const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;

    if (!parentDb)
        return result;

    try {
        std::string qualified =
            std::format("`{}.{}.{}`", parentDb->getProjectId(), name, tableName);
        std::string query = std::format("SELECT * FROM {}", qualified);

        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        if (!orderByClause.empty())
            query += " ORDER BY " + orderByClause;

        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        auto qr = parentDb->executeQuery(query, limit);
        if (!qr.statements.empty() && qr.statements[0].success) {
            result = std::move(qr.statements[0].tableData);
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting table data for {}: {}", tableName, e.what()));
    }

    return result;
}

std::vector<std::string> BigQueryDatasetNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;

    if (!parentDb)
        return columnNames;

    try {
        auto& tableClient = parentDb->getTableClient();

        google::cloud::bigquery::v2::GetTableRequest req;
        req.set_project_id(parentDb->getProjectId());
        req.set_dataset_id(name);
        req.set_table_id(tableName);

        auto result = tableClient.GetTable(req);
        if (result.ok()) {
            for (const auto& field : result->schema().fields()) {
                columnNames.push_back(field.name());
            }
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting columns for {}: {}", tableName, e.what()));
    }

    return columnNames;
}

int BigQueryDatasetNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    if (!parentDb)
        return 0;

    try {
        std::string qualified =
            std::format("`{}.{}.{}`", parentDb->getProjectId(), name, tableName);
        std::string query = std::format("SELECT COUNT(*) FROM {}", qualified);
        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        auto qr = parentDb->executeQuery(query, 1);
        if (!qr.statements.empty() && qr.statements[0].success &&
            !qr.statements[0].tableData.empty() && !qr.statements[0].tableData[0].empty()) {
            return std::stoi(qr.statements[0].tableData[0][0]);
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting row count for {}: {}", tableName, e.what()));
    }

    return 0;
}

void BigQueryDatasetNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
}

std::pair<bool, std::string> BigQueryDatasetNode::dropTable(const std::string& tableName) {
    std::string sql =
        std::format("DROP TABLE `{}.{}.{}`", parentDb->getProjectId(), name, tableName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
