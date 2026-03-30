#include "database/clickhouse/clickhouse_database_node.hpp"
#include "database/clickhouse.hpp"
#include "database/db.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <chrono>
#include <clickhouse/client.h>
#include <format>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {

    std::function<CHClientHandle()> makeClickHouseFactory(const DatabaseConnectionInfo& info) {
        return [info]() -> CHClientHandle {
            clickhouse::ClientOptions opts;
            opts.SetHost(info.host)
                .SetPort(static_cast<uint16_t>(info.port))
                .SetDefaultDatabase(info.database)
                .SetUser(info.username)
                .SetPassword(info.password)
                .SetRethrowException(true)
                .SetConnectionConnectTimeout(std::chrono::seconds(5));
            auto* client = new clickhouse::Client(opts);
            client->Ping();
            return client;
        };
    }

    std::string columnValueToString(const clickhouse::Block& block, size_t col, size_t row) {
        auto column = block[col];
        if (!column || row >= column->Size())
            return "NULL";

        try {
            auto item = column->GetItem(row);
            auto sv = item.data;

            switch (column->Type()->GetCode()) {
            case clickhouse::Type::String:
            case clickhouse::Type::FixedString:
                return std::string(sv);
            case clickhouse::Type::UInt8:
                return std::to_string(*reinterpret_cast<const uint8_t*>(sv.data()));
            case clickhouse::Type::UInt16:
                return std::to_string(*reinterpret_cast<const uint16_t*>(sv.data()));
            case clickhouse::Type::UInt32:
                return std::to_string(*reinterpret_cast<const uint32_t*>(sv.data()));
            case clickhouse::Type::UInt64:
                return std::to_string(*reinterpret_cast<const uint64_t*>(sv.data()));
            case clickhouse::Type::Int8:
                return std::to_string(*reinterpret_cast<const int8_t*>(sv.data()));
            case clickhouse::Type::Int16:
                return std::to_string(*reinterpret_cast<const int16_t*>(sv.data()));
            case clickhouse::Type::Int32:
                return std::to_string(*reinterpret_cast<const int32_t*>(sv.data()));
            case clickhouse::Type::Int64:
                return std::to_string(*reinterpret_cast<const int64_t*>(sv.data()));
            case clickhouse::Type::Float32:
                return std::to_string(*reinterpret_cast<const float*>(sv.data()));
            case clickhouse::Type::Float64:
                return std::to_string(*reinterpret_cast<const double*>(sv.data()));
            default:
                if (!sv.empty() && sv.size() <= 256)
                    return std::string(sv);
                return "(binary)";
            }
        } catch (...) {
            return "(error)";
        }
    }

    StatementResult extractClickHouseResult(clickhouse::Client* client, const std::string& query,
                                            int rowLimit) {
        StatementResult result;
        int totalRows = 0;

        client->Select(query, [&](const clickhouse::Block& block) {
            if (block.GetRowCount() == 0)
                return;

            if (result.columnNames.empty()) {
                for (size_t c = 0; c < block.GetColumnCount(); ++c)
                    result.columnNames.push_back(block.GetColumnName(c));
            }

            for (size_t r = 0; r < block.GetRowCount() && totalRows < rowLimit; ++r) {
                std::vector<std::string> rowData;
                rowData.reserve(block.GetColumnCount());
                for (size_t c = 0; c < block.GetColumnCount(); ++c)
                    rowData.push_back(columnValueToString(block, c, r));
                result.tableData.push_back(std::move(rowData));
                ++totalRows;
            }
        });

        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        if (totalRows >= rowLimit)
            result.message += std::format(" (limited to {})", rowLimit);

        return result;
    }

} // namespace

void ClickHouseDatabaseNode::ensureConnectionPool() {
    if (!connectionPool && parentDb) {
        auto nodeInfo = parentDb->getConnectionInfo();
        nodeInfo.database = name;
        initializeConnectionPool(nodeInfo);
    }
}

void ClickHouseDatabaseNode::initializeConnectionPool(const DatabaseConnectionInfo& info) {
    if (!parentDb || connectionPool)
        return;

    connectionPool = std::make_unique<ConnectionPool<CHClientHandle>>(
        makeClickHouseFactory(info),
        [](CHClientHandle h) { delete static_cast<clickhouse::Client*>(h); },
        [](CHClientHandle h) -> bool {
            try {
                static_cast<clickhouse::Client*>(h)->Ping();
                return true;
            } catch (...) {
                return false;
            }
        });
}

ConnectionPool<CHClientHandle>::Session ClickHouseDatabaseNode::getSession() const {
    if (!connectionPool)
        throw std::runtime_error(
            "ClickHouseDatabaseNode::getSession: Connection pool not available for database: " +
            name);
    return connectionPool->acquire();
}

void ClickHouseDatabaseNode::startTablesLoadAsync(bool forceRefresh) {
    if (!parentDb || tablesLoader.isRunning())
        return;

    if (forceRefresh) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }

    if (!forceRefresh && tablesLoaded)
        return;

    tables.clear();
    tablesLoader.start([this]() { return getTablesAsync(); });
}

std::vector<Table> ClickHouseDatabaseNode::getTablesAsync() {
    std::vector<Table> result;

    if (!tablesLoader.isRunning())
        return result;

    try {
        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::vector<std::string> tableNames;
        std::string listQuery = std::format(
            "SELECT name FROM system.tables WHERE database = '{}' AND engine != 'View'", name);

        client->Select(listQuery, [&](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r) {
                if (!tablesLoader.isRunning())
                    return;
                tableNames.push_back(columnValueToString(block, 0, r));
            }
        });

        spdlog::debug("found {} tables in ClickHouse database {}", tableNames.size(), name);

        for (const auto& tableName : tableNames) {
            if (!tablesLoader.isRunning())
                break;

            Table table;
            table.name = tableName;
            table.fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;

            std::string descQuery = std::format("DESCRIBE TABLE `{}`.`{}`", name, tableName);
            client->Select(descQuery, [&](const clickhouse::Block& block) {
                for (size_t r = 0; r < block.GetRowCount(); ++r) {
                    if (!tablesLoader.isRunning())
                        return;
                    Column col;
                    col.name = columnValueToString(block, 0, r);
                    col.type = columnValueToString(block, 1, r);
                    table.columns.push_back(col);
                }
            });

            result.push_back(std::move(table));
        }
    } catch (const std::exception& e) {
        spdlog::error("error getting tables for ClickHouse database {}: {}", name, e.what());
        lastTablesError = e.what();
    }

    return result;
}

void ClickHouseDatabaseNode::startViewsLoadAsync(bool forceRefresh) {
    if (!parentDb || viewsLoader.isRunning())
        return;

    if (viewsLoader.isRunning() || (viewsLoaded && !forceRefresh))
        return;

    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    viewsLoader.start([this]() { return getViewsForDatabaseAsync(); });
}

std::vector<Table> ClickHouseDatabaseNode::getViewsForDatabaseAsync() {
    std::vector<Table> result;

    if (!viewsLoader.isRunning())
        return result;

    try {
        if (!connectionPool)
            ensureConnectionPool();

        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::vector<std::string> viewNames;
        std::string listQuery = std::format(
            "SELECT name FROM system.tables WHERE database = '{}' AND engine = 'View'", name);

        client->Select(listQuery, [&](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r) {
                if (!viewsLoader.isRunning())
                    return;
                viewNames.push_back(columnValueToString(block, 0, r));
            }
        });

        spdlog::debug("found {} views in ClickHouse database {}", viewNames.size(), name);

        for (const auto& viewName : viewNames) {
            if (!viewsLoader.isRunning())
                break;

            Table view;
            view.name = viewName;
            view.fullName = parentDb->getConnectionInfo().name + "." + name + "." + viewName;

            std::string descQuery = std::format("DESCRIBE TABLE `{}`.`{}`", name, viewName);
            client->Select(descQuery, [&](const clickhouse::Block& block) {
                for (size_t r = 0; r < block.GetRowCount(); ++r) {
                    if (!viewsLoader.isRunning())
                        return;
                    Column col;
                    col.name = columnValueToString(block, 0, r);
                    col.type = columnValueToString(block, 1, r);
                    view.columns.push_back(col);
                }
            });

            result.push_back(std::move(view));
        }
    } catch (const std::exception& e) {
        spdlog::error("error getting views for ClickHouse database {}: {}", name, e.what());
        lastViewsError = e.what();
    }

    return result;
}

void ClickHouseDatabaseNode::checkLoadingStatus() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        tablesLoaded = true;
    });
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        viewsLoaded = true;
    });
}

void ClickHouseDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning())
        return;
    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

bool ClickHouseDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

void ClickHouseDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end())
        return;

    it->second.check([this, tableName](const Table& refreshedTable) {
        const auto tableIt = std::ranges::find_if(
            tables, [&tableName](const Table& t) { return t.name == tableName; });
        if (tableIt != tables.end())
            *tableIt = refreshedTable;
        tableRefreshLoaders.erase(tableName);
    });
}

Table ClickHouseDatabaseNode::refreshTableAsync(const std::string& tableName) {
    Table refreshedTable;
    refreshedTable.name = tableName;
    refreshedTable.fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;

    try {
        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::string descQuery = std::format("DESCRIBE TABLE `{}`.`{}`", name, tableName);
        client->Select(descQuery, [&](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r) {
                Column col;
                col.name = columnValueToString(block, 0, r);
                col.type = columnValueToString(block, 1, r);
                refreshedTable.columns.push_back(col);
            }
        });
    } catch (const std::exception& e) {
        spdlog::error("error refreshing ClickHouse table {}: {}", tableName, e.what());
        throw;
    }

    return refreshedTable;
}

std::vector<std::vector<std::string>>
ClickHouseDatabaseNode::getTableData(const std::string& tableName, const int limit,
                                     const int offset, const std::string& whereClause,
                                     const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;

    try {
        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::string query = std::format("SELECT * FROM `{}`.`{}`", name, tableName);
        if (!whereClause.empty())
            query += " WHERE " + whereClause;
        if (!orderByClause.empty())
            query += " ORDER BY " + orderByClause;
        query += std::format(" LIMIT {} OFFSET {}", limit, offset);

        client->Select(query, [&](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r) {
                std::vector<std::string> rowData;
                rowData.reserve(block.GetColumnCount());
                for (size_t c = 0; c < block.GetColumnCount(); ++c)
                    rowData.push_back(columnValueToString(block, c, r));
                result.push_back(std::move(rowData));
            }
        });
    } catch (const std::exception& e) {
        spdlog::error("error getting table data for {}: {}", tableName, e.what());
    }

    return result;
}

std::vector<std::string> ClickHouseDatabaseNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;

    try {
        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::string query = std::format("DESCRIBE TABLE `{}`.`{}`", name, tableName);
        client->Select(query, [&](const clickhouse::Block& block) {
            for (size_t r = 0; r < block.GetRowCount(); ++r)
                columnNames.push_back(columnValueToString(block, 0, r));
        });
    } catch (const std::exception& e) {
        spdlog::error("error getting column names for {}: {}", tableName, e.what());
    }

    return columnNames;
}

int ClickHouseDatabaseNode::getRowCount(const std::string& tableName,
                                        const std::string& whereClause) {
    int count = 0;

    try {
        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::string query = std::format("SELECT count() FROM `{}`.`{}`", name, tableName);
        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        client->Select(query, [&](const clickhouse::Block& block) {
            if (block.GetRowCount() > 0) {
                std::string val = columnValueToString(block, 0, 0);
                count = std::atoi(val.c_str());
            }
        });
    } catch (const std::exception& e) {
        spdlog::error("error getting row count for {}: {}", tableName, e.what());
    }

    return count;
}

QueryResult ClickHouseDatabaseNode::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        auto session = getSession();
        auto* client = static_cast<clickhouse::Client*>(session.get());

        std::string trimmed = query;
        auto pos = trimmed.find_first_not_of(" \t\n\r");
        if (pos != std::string::npos)
            trimmed = trimmed.substr(pos);

        bool isSelect = trimmed.size() >= 4 && (strncasecmp(trimmed.c_str(), "select", 6) == 0 ||
                                                strncasecmp(trimmed.c_str(), "show", 4) == 0 ||
                                                strncasecmp(trimmed.c_str(), "describe", 8) == 0 ||
                                                strncasecmp(trimmed.c_str(), "explain", 7) == 0 ||
                                                strncasecmp(trimmed.c_str(), "with", 4) == 0);

        if (isSelect) {
            auto r = extractClickHouseResult(client, query, rowLimit);
            result.statements.push_back(std::move(r));
        } else {
            clickhouse::Query q(query);
            client->Execute(q);
            StatementResult r;
            r.message = "Query executed successfully";
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

std::pair<bool, std::string> ClickHouseDatabaseNode::createTable(const Table& table) {
    try {
        const auto builder = createSQLBuilder(getDatabaseType());
        std::string sql = builder->createTable(table, name);
        auto result = executeQuery(sql);
        if (!result.success())
            return {false, result.errorMessage()};
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

DatabaseInterface* ClickHouseDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string ClickHouseDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType ClickHouseDatabaseNode::getDatabaseType() const {
    return DatabaseType::CLICKHOUSE;
}

std::pair<bool, std::string> ClickHouseDatabaseNode::renameTable(const std::string& oldName,
                                                                 const std::string& newName) {
    auto sql = std::format("RENAME TABLE `{}`.`{}` TO `{}`.`{}`", name, oldName, name, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> ClickHouseDatabaseNode::dropTable(const std::string& tableName) {
    auto sql = std::format("DROP TABLE `{}`.`{}`", name, tableName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> ClickHouseDatabaseNode::truncateTable(const std::string& tableName) {
    auto sql = std::format("TRUNCATE TABLE `{}`.`{}`", name, tableName);
    auto r = executeQuery(sql);
    if (r.success())
        return {true, ""};
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> ClickHouseDatabaseNode::dropColumn(const std::string& tableName,
                                                                const std::string& columnName) {
    auto sql = std::format("ALTER TABLE `{}`.`{}` DROP COLUMN `{}`", name, tableName, columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
