#include "database/duckdb.hpp"
#include "database/ddl_builder.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <format>

namespace {
    std::vector<std::vector<std::string>>
    queryRows(duckdb_connection conn, const std::string& sql,
              std::vector<std::string>* columnNames = nullptr) {
        std::vector<std::vector<std::string>> rows;
        duckdb_result result;
        if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
            throw std::runtime_error(duckdb_result_error(&result));
        }

        const idx_t colCount = duckdb_column_count(&result);
        const idx_t rowCount = duckdb_row_count(&result);

        if (columnNames) {
            columnNames->clear();
            for (idx_t col = 0; col < colCount; ++col) {
                columnNames->emplace_back(duckdb_column_name(&result, col));
            }
        }

        for (idx_t row = 0; row < rowCount; ++row) {
            std::vector<std::string> rowData;
            for (idx_t col = 0; col < colCount; ++col) {
                if (duckdb_value_is_null(&result, col, row)) {
                    rowData.emplace_back("NULL");
                    continue;
                }
                auto* str = duckdb_value_varchar(&result, col, row);
                rowData.emplace_back(str ? str : "");
                duckdb_free(str);
            }
            rows.push_back(std::move(rowData));
        }

        duckdb_destroy_result(&result);
        return rows;
    }
} // namespace

DuckDBDatabase::DuckDBDatabase(const DatabaseConnectionInfo& connInfo) {
    connectionInfo = connInfo;
}

DuckDBDatabase::~DuckDBDatabase() {
    disconnect();
}

std::pair<bool, std::string> DuckDBDatabase::connect() {
    if (connected && conn_) {
        return {true, ""};
    }

    if (duckdb_open(connectionInfo.path.c_str(), &db_) != DuckDBSuccess) {
        return {false, "Failed to open DuckDB database"};
    }
    if (duckdb_connect(db_, &conn_) != DuckDBSuccess) {
        duckdb_close(&db_);
        return {false, "Failed to connect to DuckDB database"};
    }

    connected = true;
    return {true, ""};
}

void DuckDBDatabase::disconnect() {
    if (conn_) {
        duckdb_disconnect(&conn_);
    }
    if (db_) {
        duckdb_close(&db_);
    }
    connected = false;
}

std::string DuckDBDatabase::getName() const {
    const auto& path = connectionInfo.path;
    auto pos = path.find_last_of("/\\");
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
}

std::string DuckDBDatabase::getFullPath() const {
    return connectionInfo.path;
}

QueryResult DuckDBDatabase::executeQuery(const std::string& sql, int limit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connected || !conn_) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Database not connected";
        result.statements.push_back(r);
        return result;
    }

    StatementResult r;
    try {
        std::vector<std::string> columnNames;
        auto rows = queryRows(conn_, sql, &columnNames);
        r.columnNames = std::move(columnNames);

        if (!r.columnNames.empty()) {
            const auto maxRows = std::min<size_t>(rows.size(), static_cast<size_t>(limit));
            r.tableData.assign(rows.begin(), rows.begin() + static_cast<long>(maxRows));
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            r.message = "Query executed successfully";
        }
    } catch (const std::exception& e) {
        r.success = false;
        r.errorMessage = e.what();
    }

    result.statements.push_back(std::move(r));
    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

std::pair<bool, std::string> DuckDBDatabase::createTable(const Table& table) {
    DDLBuilder builder(DatabaseType::DUCKDB);
    auto result = executeQuery(builder.createTable(table));
    return result.success() ? std::pair{true, ""} : std::pair{false, result.errorMessage()};
}

std::vector<std::string> DuckDBDatabase::getColumnNames(const std::string& tableName) {
    std::vector<std::string> cols;
    try {
        auto rows = queryRows(conn_, "PRAGMA table_info('" + tableName + "')");
        for (const auto& row : rows) {
            if (row.size() > 1)
                cols.push_back(row[1]);
        }
    } catch (...) {
    }
    return cols;
}

std::vector<std::vector<std::string>>
DuckDBDatabase::getTableData(const std::string& tableName, int limit, int offset,
                             const std::string& whereClause, const std::string& orderByClause) {
    std::string sql = std::format("SELECT * FROM {}", tableName);
    if (!whereClause.empty())
        sql += std::format(" WHERE {}", whereClause);
    if (!orderByClause.empty())
        sql += std::format(" ORDER BY {}", orderByClause);
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);

    try {
        return queryRows(conn_, sql);
    } catch (...) {
        return {};
    }
}

int DuckDBDatabase::getRowCount(const std::string& tableName, const std::string& whereClause) {
    try {
        auto sql = whereClause.empty()
                       ? std::format("SELECT COUNT(*) FROM {}", tableName)
                       : std::format("SELECT COUNT(*) FROM {} WHERE {}", tableName, whereClause);
        auto rows = queryRows(conn_, sql);
        if (!rows.empty() && !rows[0].empty())
            return std::stoi(rows[0][0]);
    } catch (...) {
    }
    return 0;
}

Table DuckDBDatabase::loadTableMetadata(const std::string& tableName) const {
    Table table;
    table.name = tableName;
    table.fullName = connectionInfo.name + "." + tableName;

    auto cols = queryRows(conn_, "PRAGMA table_info('" + tableName + "')");
    for (const auto& row : cols) {
        if (row.size() < 6)
            continue;
        Column col;
        col.name = row[1];
        col.type = row[2];
        col.isNotNull = row[3] == "true" || row[3] == "1";
        col.isPrimaryKey = row[5] == "true" || row[5] == "1";
        table.columns.push_back(std::move(col));
    }

    buildForeignKeyLookup(table);
    return table;
}

std::vector<Table> DuckDBDatabase::getTablesAsync() const {
    std::vector<Table> out;
    auto names = queryRows(
        conn_, "SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' AND "
               "table_type='BASE TABLE' ORDER BY table_name");
    for (const auto& row : names) {
        if (row.empty())
            continue;
        out.push_back(loadTableMetadata(row[0]));
    }
    populateIncomingForeignKeys(out);
    return out;
}

std::vector<Table> DuckDBDatabase::getViewsAsync() const {
    std::vector<Table> out;
    auto names = queryRows(
        conn_, "SELECT table_name FROM information_schema.tables WHERE table_schema = 'main' AND "
               "table_type='VIEW' ORDER BY table_name");
    for (const auto& row : names) {
        if (row.empty())
            continue;
        out.push_back(loadTableMetadata(row[0]));
    }
    return out;
}

void DuckDBDatabase::startTablesLoadAsync(bool force) {
    if (force) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }
    if (!force && tablesLoaded)
        return;
    tablesLoader.start([this]() { return getTablesAsync(); });
}

void DuckDBDatabase::startViewsLoadAsync(bool force) {
    if (force) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }
    if (!force && viewsLoaded)
        return;
    viewsLoader.start([this]() { return getViewsAsync(); });
}

void DuckDBDatabase::checkLoadingStatus() {
    tablesLoader.check([this](std::vector<Table> result) {
        tables = std::move(result);
        tablesLoaded = true;
    });
    viewsLoader.check([this](std::vector<Table> result) {
        views = std::move(result);
        viewsLoaded = true;
    });

    for (auto it = tableRefreshLoaders.begin(); it != tableRefreshLoaders.end();) {
        const auto tableName = it->first;
        it->second.check([this, tableName](Table refreshed) {
            auto found = std::find_if(tables.begin(), tables.end(),
                                      [&](const Table& t) { return t.name == tableName; });
            if (found != tables.end()) {
                *found = std::move(refreshed);
            }
        });
        if (!it->second.isRunning()) {
            it = tableRefreshLoaders.erase(it);
        } else {
            ++it;
        }
    }
}

void DuckDBDatabase::startTableRefreshAsync(const std::string& tableName) {
    auto& loader = tableRefreshLoaders[tableName];
    loader.start([this, tableName]() { return loadTableMetadata(tableName); });
}

bool DuckDBDatabase::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}
