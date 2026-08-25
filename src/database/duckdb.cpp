#include "database/duckdb.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <spdlog/spdlog.h>

namespace {
    // duck-typed RAII for duckdb_result
    struct ResultGuard {
        duckdb_result res{};
        ~ResultGuard() {
            duckdb_destroy_result(&res);
        }
    };

    std::string cellText(duckdb_result* res, idx_t col, idx_t row) {
        if (duckdb_value_is_null(res, col, row)) {
            return std::string(NULL_SENTINEL);
        }
        char* v = duckdb_value_varchar(res, col, row);
        std::string out = v ? v : "";
        duckdb_free(v);
        return out;
    }

    std::string escapeSqlLiteral(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '\'')
                out += "''";
            else
                out += c;
        }
        return out;
    }

    bool boolText(const std::string& v) {
        return v == "true" || v == "1";
    }
} // namespace

DuckDBDatabase::DuckDBDatabase(const DatabaseConnectionInfo& connInfo) {
    connectionInfo = connInfo;
}

DuckDBDatabase::~DuckDBDatabase() {
    DuckDBDatabase::disconnect();
}

bool DuckDBDatabase::isCsvPath(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".csv";
}

std::pair<bool, std::string> DuckDBDatabase::connect() {
    if (connected && con_) {
        return {true, ""};
    }

    // csv files are imported into an in-memory database so they can be queried with SQL
    const bool csv = isCsvPath(connectionInfo.path);
    const std::string openPath = csv ? ":memory:" : connectionInfo.path;

    char* errMsg = nullptr;
    if (duckdb_open_ext(openPath.c_str(), &db_, nullptr, &errMsg) == DuckDBError) {
        std::string error = errMsg ? errMsg : "Unable to open database";
        duckdb_free(errMsg);
        db_ = nullptr;
        spdlog::error("Can't open DuckDB database: {}", error);
        return {false, error};
    }
    duckdb_free(errMsg);

    if (duckdb_connect(db_, &con_) == DuckDBError) {
        duckdb_close(&db_);
        db_ = nullptr;
        con_ = nullptr;
        return {false, "Failed to create DuckDB connection"};
    }

    connected = true;

    if (csv) {
        // ponytail: materialized copy in memory; Refresh re-imports the file
        std::string table = std::filesystem::path(connectionInfo.path).stem().string();
        std::string quoted = "\"";
        for (char c : table) {
            quoted += (c == '"') ? "\"\"" : std::string(1, c);
        }
        quoted += "\"";
        auto out = runQuery(std::format("CREATE TABLE {} AS SELECT * FROM read_csv('{}')", quoted,
                                        escapeSqlLiteral(connectionInfo.path)));
        if (!out.ok) {
            disconnect();
            return {false, "Failed to import CSV: " + out.error};
        }
    }

    spdlog::info("Successfully connected to DuckDB database: {}", connectionInfo.path);
    return {true, ""};
}

void DuckDBDatabase::disconnect() {
    connected = false; // reject new queries before tearing down the handles
    std::lock_guard lock(queryMutex_);
    if (con_) {
        duckdb_disconnect(&con_);
        con_ = nullptr;
    }
    if (db_) {
        duckdb_close(&db_);
        db_ = nullptr;
    }
    connected = false;
}

DuckDBDatabase::QueryOutput DuckDBDatabase::runQuery(const std::string& sql, int rowLimit) const {
    QueryOutput out;
    std::lock_guard lock(queryMutex_);
    if (!connected || !con_) {
        out.error = "Database not connected";
        return out;
    }

    ResultGuard guard;
    if (duckdb_query(con_, sql.c_str(), &guard.res) == DuckDBError) {
        const char* err = duckdb_result_error(&guard.res);
        out.error = err ? err : "Query failed";
        return out;
    }

    const idx_t colCount = duckdb_column_count(&guard.res);
    const idx_t rowCount = duckdb_row_count(&guard.res);
    for (idx_t c = 0; c < colCount; ++c) {
        const char* name = duckdb_column_name(&guard.res, c);
        out.columns.emplace_back(name ? name : "");
    }
    const idx_t maxRows =
        rowLimit < 0 ? rowCount : std::min<idx_t>(rowCount, static_cast<idx_t>(rowLimit));
    for (idx_t r = 0; r < maxRows; ++r) {
        std::vector<std::string> rowData;
        rowData.reserve(colCount);
        for (idx_t c = 0; c < colCount; ++c) {
            rowData.push_back(cellText(&guard.res, c, r));
        }
        out.rows.push_back(std::move(rowData));
    }
    out.affectedRows = static_cast<long long>(duckdb_rows_changed(&guard.res));
    out.ok = true;
    return out;
}

QueryResult DuckDBDatabase::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    std::lock_guard lock(queryMutex_);

    if (!connected || !con_) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Database not connected";
        result.statements.push_back(r);
        return result;
    }

    duckdb_extracted_statements stmts = nullptr;
    const idx_t count = duckdb_extract_statements(con_, query.c_str(), &stmts);
    if (count == 0) {
        StatementResult r;
        r.success = false;
        const char* err = stmts ? duckdb_extract_statements_error(stmts) : nullptr;
        r.errorMessage = err ? err : "Failed to parse query";
        result.statements.push_back(r);
        duckdb_destroy_extracted(&stmts);
        return result;
    }

    for (idx_t i = 0; i < count; ++i) {
        StatementResult r;

        duckdb_prepared_statement prep = nullptr;
        if (duckdb_prepare_extracted_statement(con_, stmts, i, &prep) == DuckDBError) {
            const char* err = duckdb_prepare_error(prep);
            r.success = false;
            r.errorMessage = err ? err : "Failed to prepare statement";
            duckdb_destroy_prepare(&prep);
            result.statements.push_back(std::move(r));
            break;
        }

        ResultGuard guard;
        if (duckdb_execute_prepared(prep, &guard.res) == DuckDBError) {
            const char* err = duckdb_result_error(&guard.res);
            r.success = false;
            r.errorMessage = err ? err : "Query failed";
            duckdb_destroy_prepare(&prep);
            result.statements.push_back(std::move(r));
            break;
        }
        duckdb_destroy_prepare(&prep);

        const idx_t colCount = duckdb_column_count(&guard.res);
        const idx_t rowCount = duckdb_row_count(&guard.res);
        // DML results also carry a "Count" column, so classify by return type
        if (duckdb_result_return_type(guard.res) == DUCKDB_RESULT_TYPE_QUERY_RESULT) {
            for (idx_t c = 0; c < colCount; ++c) {
                const char* name = duckdb_column_name(&guard.res, c);
                r.columnNames.emplace_back(name ? name : "");
            }
            const idx_t maxRows = std::min<idx_t>(rowCount, static_cast<idx_t>(rowLimit));
            for (idx_t row = 0; row < maxRows; ++row) {
                std::vector<std::string> rowData;
                rowData.reserve(colCount);
                for (idx_t c = 0; c < colCount; ++c) {
                    rowData.push_back(cellText(&guard.res, c, row));
                }
                r.tableData.push_back(std::move(rowData));
            }
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            r.affectedRows = static_cast<int>(duckdb_rows_changed(&guard.res));
            r.message = "Query executed successfully";
        }

        result.statements.push_back(std::move(r));
    }

    duckdb_destroy_extracted(&stmts);

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    return result;
}

std::pair<bool, std::string> DuckDBDatabase::createTable(const Table& table) {
    if (!connected || !con_) {
        return {false, "Database not connected"};
    }

    try {
        const auto builder = createSQLBuilder(DatabaseType::DUCKDB);
        auto result = executeQuery(builder->createTable(table));
        if (!result.success()) {
            return {false, result.errorMessage()};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

std::vector<std::vector<std::string>> DuckDBDatabase::getTableData(const std::string& tableName,
                                                                   int limit, int offset) {
    Table t;
    t.name = tableName;
    return getTableData(t, limit, offset, "");
}

std::vector<std::vector<std::string>>
DuckDBDatabase::getTableData(const Table& table, int limit, int offset,
                             const std::string& whereClause, const std::string& orderByClause) {
    const auto builder = createSQLBuilder(DatabaseType::DUCKDB);
    auto out = runQuery(builder->selectAll(table, whereClause, orderByClause, limit, offset));
    if (!out.ok) {
        spdlog::error("Error getting table data: {}", out.error);
    }
    return std::move(out.rows);
}

std::vector<std::string> DuckDBDatabase::getColumnNames(const Table& table) {
    const auto builder = createSQLBuilder(DatabaseType::DUCKDB);
    auto out = runQuery(builder->columnNames(table));
    if (!out.ok) {
        spdlog::error("Error getting column names: {}", out.error);
        return {};
    }
    std::vector<std::string> names;
    names.reserve(out.rows.size());
    for (const auto& row : out.rows) {
        if (!row.empty())
            names.push_back(row[0]);
    }
    return names;
}

int DuckDBDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    const auto builder = createSQLBuilder(DatabaseType::DUCKDB);
    auto out = runQuery(builder->countRows(table, whereClause));
    if (!out.ok || out.rows.empty() || out.rows[0].empty()) {
        return 0;
    }
    try {
        return std::stoi(out.rows[0][0]);
    } catch (...) {
        return 0;
    }
}

Table DuckDBDatabase::loadTableMeta(const std::string& tableName) const {
    Table table;
    table.name = tableName;
    table.fullName = connectionInfo.name + "." + tableName;

    const std::string escaped = escapeSqlLiteral(tableName);

    auto cols = runQuery(std::format("PRAGMA table_info('{}')", escaped));
    for (const auto& row : cols.rows) {
        if (row.size() < 6)
            continue;
        Column col;
        col.name = row[1];
        col.type = row[2];
        col.isNotNull = boolText(row[3]);
        col.isPrimaryKey = boolText(row[5]);
        table.columns.push_back(std::move(col));
    }

    auto idx = runQuery(std::format(
        "SELECT index_name, is_unique FROM duckdb_indexes() WHERE table_name = '{}'", escaped));
    for (const auto& row : idx.rows) {
        if (row.size() < 2)
            continue;
        Index index;
        index.name = row[0];
        index.isUnique = boolText(row[1]);
        table.indexes.push_back(std::move(index));
    }

    // ponytail: foreign keys skipped — duckdb_constraints() exposes them as list
    // values; parse constraint_column_names when FK arrows are wanted in diagrams
    buildForeignKeyLookup(table);
    return table;
}

std::vector<Table> DuckDBDatabase::getTablesAsync() const {
    std::vector<Table> result;

    auto names = runQuery("SELECT table_name FROM information_schema.tables "
                          "WHERE table_type = 'BASE TABLE' AND table_schema = 'main' "
                          "ORDER BY table_name");
    if (!names.ok) {
        spdlog::error("Error loading tables: {}", names.error);
        return result;
    }

    for (const auto& row : names.rows) {
        if (!row.empty())
            result.push_back(loadTableMeta(row[0]));
    }
    populateIncomingForeignKeys(result);
    spdlog::debug("Finished loading tables. Total tables: {}", result.size());
    return result;
}

std::vector<Table> DuckDBDatabase::getViewsAsync() const {
    std::vector<Table> result;

    auto names = runQuery("SELECT table_name FROM information_schema.tables "
                          "WHERE table_type = 'VIEW' AND table_schema = 'main' "
                          "ORDER BY table_name");
    if (!names.ok) {
        spdlog::error("Error loading views: {}", names.error);
        return result;
    }

    for (const auto& row : names.rows) {
        if (row.empty())
            continue;
        Table view;
        view.name = row[0];
        view.fullName = connectionInfo.name + "." + view.name;

        auto cols = runQuery(std::format("PRAGMA table_info('{}')", escapeSqlLiteral(view.name)));
        for (const auto& colRow : cols.rows) {
            if (colRow.size() < 4)
                continue;
            Column col;
            col.name = colRow[1];
            col.type = colRow[2];
            col.isNotNull = boolText(colRow[3]);
            view.columns.push_back(std::move(col));
        }
        result.push_back(std::move(view));
    }
    spdlog::debug("Finished loading views. Total views: {}", result.size());
    return result;
}

std::vector<std::string> DuckDBDatabase::getSequencesAsync() const {
    std::vector<std::string> result;

    auto out = runQuery("SELECT sequence_name FROM duckdb_sequences() ORDER BY sequence_name");
    if (!out.ok) {
        spdlog::error("Error loading sequences: {}", out.error);
        return result;
    }
    for (const auto& row : out.rows) {
        if (!row.empty())
            result.push_back(row[0]);
    }
    return result;
}

void DuckDBDatabase::startTableRefreshAsync(const std::string& tableName) {
    auto& loader = tableRefreshLoaders[tableName];
    loader.start([this, tableName]() { return loadTableMeta(tableName); });
}
