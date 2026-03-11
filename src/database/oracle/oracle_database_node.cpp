#include "database/oracle/oracle_database_node.hpp"
#include "database/db.hpp"
#include "database/ddl_builder.hpp"
#include "database/oracle.hpp"
#include "oracle_utils.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <ranges>

namespace {

    // load columns for a table/view in schema
    std::vector<Column> loadColumns(dpiConn* conn, const std::string& schema,
                                    const std::string& tableName) {
        std::vector<Column> columns;
        std::string query = std::format(
            "SELECT COLUMN_NAME, DATA_TYPE, NULLABLE, DATA_LENGTH, DATA_PRECISION, DATA_SCALE "
            "FROM ALL_TAB_COLUMNS "
            "WHERE OWNER = '{}' AND TABLE_NAME = '{}' "
            "ORDER BY COLUMN_ID",
            schema, tableName);

        dpiContext* ctx = OracleDatabase::getContext();
        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS) {
            return columns;
        }

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS || numCols == 0) {
            dpiStmt_release(stmt);
            return columns;
        }

        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            Column col;

            dpiNativeTypeNum nt;
            dpiData* d;

            // COLUMN_NAME
            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            col.name = dpiDataToString(nt, d);

            // DATA_TYPE
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            std::string dataType = dpiDataToString(nt, d);

            // NULLABLE
            dpiStmt_getQueryValue(stmt, 3, &nt, &d);
            std::string nullable = dpiDataToString(nt, d);
            col.isNotNull = (nullable == "N");

            // DATA_LENGTH
            dpiStmt_getQueryValue(stmt, 4, &nt, &d);
            std::string dataLen = dpiDataToString(nt, d);

            // DATA_PRECISION
            dpiStmt_getQueryValue(stmt, 5, &nt, &d);
            std::string prec = dpiDataToString(nt, d);

            // DATA_SCALE
            dpiStmt_getQueryValue(stmt, 6, &nt, &d);
            std::string scale = dpiDataToString(nt, d);

            // format type with precision/scale
            if (prec != "NULL") {
                if (scale != "NULL" && scale != "0") {
                    col.type = std::format("{}({},{})", dataType, prec, scale);
                } else {
                    col.type = std::format("{}({})", dataType, prec);
                }
            } else if (dataType == "VARCHAR2" || dataType == "CHAR" || dataType == "NVARCHAR2" ||
                       dataType == "RAW") {
                col.type = std::format("{}({})", dataType, dataLen);
            } else {
                col.type = dataType;
            }

            columns.push_back(col);
        }

        dpiStmt_release(stmt);
        return columns;
    }

    // mark primary key columns
    void loadPrimaryKeys(dpiConn* conn, const std::string& schema, const std::string& tableName,
                         std::vector<Column>& columns) {
        std::string query = std::format(
            "SELECT acc.COLUMN_NAME "
            "FROM ALL_CONS_COLUMNS acc "
            "JOIN ALL_CONSTRAINTS ac ON acc.CONSTRAINT_NAME = ac.CONSTRAINT_NAME "
            "AND acc.OWNER = ac.OWNER "
            "WHERE ac.CONSTRAINT_TYPE = 'P' AND ac.TABLE_NAME = '{}' AND ac.OWNER = '{}'",
            tableName, schema);

        auto pkCols = dpiQueryStringList(conn, query);
        for (const auto& pkCol : pkCols) {
            for (auto& col : columns) {
                if (col.name == pkCol) {
                    col.isPrimaryKey = true;
                    break;
                }
            }
        }
    }

    // load foreign keys for a table
    std::vector<ForeignKey> loadForeignKeys(dpiConn* conn, const std::string& schema,
                                            const std::string& tableName) {
        std::vector<ForeignKey> fks;
        std::string query = std::format(
            "SELECT ac.CONSTRAINT_NAME, acc.COLUMN_NAME, "
            "rc.TABLE_NAME, rcc.COLUMN_NAME "
            "FROM ALL_CONSTRAINTS ac "
            "JOIN ALL_CONS_COLUMNS acc ON ac.CONSTRAINT_NAME = acc.CONSTRAINT_NAME "
            "AND ac.OWNER = acc.OWNER "
            "JOIN ALL_CONSTRAINTS rc ON ac.R_CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
            "AND ac.R_OWNER = rc.OWNER "
            "JOIN ALL_CONS_COLUMNS rcc ON rc.CONSTRAINT_NAME = rcc.CONSTRAINT_NAME "
            "AND rc.OWNER = rcc.OWNER AND acc.POSITION = rcc.POSITION "
            "WHERE ac.CONSTRAINT_TYPE = 'R' AND ac.TABLE_NAME = '{}' AND ac.OWNER = '{}' "
            "ORDER BY ac.CONSTRAINT_NAME, acc.POSITION",
            tableName, schema);

        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS) {
            return fks;
        }

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS) {
            dpiStmt_release(stmt);
            return fks;
        }

        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            ForeignKey fk;
            dpiNativeTypeNum nt;
            dpiData* d;

            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            fk.name = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            fk.sourceColumn = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 3, &nt, &d);
            fk.targetTable = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 4, &nt, &d);
            fk.targetColumn = dpiDataToString(nt, d);

            fks.push_back(fk);
        }

        dpiStmt_release(stmt);
        return fks;
    }

    // load indexes for a table
    std::vector<Index> loadIndexes(dpiConn* conn, const std::string& schema,
                                   const std::string& tableName) {
        std::vector<Index> indexes;
        std::string query = std::format(
            "SELECT i.INDEX_NAME, i.UNIQUENESS, ic.COLUMN_NAME "
            "FROM ALL_INDEXES i "
            "JOIN ALL_IND_COLUMNS ic ON i.INDEX_NAME = ic.INDEX_NAME AND i.OWNER = ic.INDEX_OWNER "
            "LEFT JOIN ALL_CONSTRAINTS c ON i.INDEX_NAME = c.INDEX_NAME AND i.OWNER = c.OWNER "
            "WHERE i.TABLE_NAME = '{}' AND i.OWNER = '{}' "
            "AND (c.CONSTRAINT_TYPE IS NULL OR c.CONSTRAINT_TYPE != 'P') "
            "ORDER BY i.INDEX_NAME, ic.COLUMN_POSITION",
            tableName, schema);

        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS) {
            return indexes;
        }

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS) {
            dpiStmt_release(stmt);
            return indexes;
        }

        std::map<std::string, Index> indexMap;
        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            dpiNativeTypeNum nt;
            dpiData* d;

            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            std::string idxName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            std::string uniqueness = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 3, &nt, &d);
            std::string colName = dpiDataToString(nt, d);

            if (!indexMap.contains(idxName)) {
                Index idx;
                idx.name = idxName;
                idx.isUnique = (uniqueness == "UNIQUE");
                indexMap[idxName] = idx;
            }
            indexMap[idxName].columns.push_back(colName);
        }

        for (auto& idx : indexMap | std::views::values) {
            indexes.push_back(std::move(idx));
        }

        dpiStmt_release(stmt);
        return indexes;
    }

    // load all metadata for a single table
    Table loadTableMetadata(dpiConn* conn, const std::string& schema, const std::string& tableName,
                            const std::string& fullName) {
        Table table;
        table.name = tableName;
        table.fullName = fullName;
        table.columns = loadColumns(conn, schema, tableName);
        loadPrimaryKeys(conn, schema, tableName, table.columns);
        table.foreignKeys = loadForeignKeys(conn, schema, tableName);
        table.indexes = loadIndexes(conn, schema, tableName);
        return table;
    }

    // batch-load all columns for all tables/views in a schema (single query)
    std::unordered_map<std::string, std::vector<Column>>
    batchLoadColumns(dpiConn* conn, const std::string& schema) {
        std::unordered_map<std::string, std::vector<Column>> result;
        std::string query =
            std::format("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, NULLABLE, DATA_LENGTH, "
                        "DATA_PRECISION, DATA_SCALE "
                        "FROM ALL_TAB_COLUMNS "
                        "WHERE OWNER = '{}' ORDER BY TABLE_NAME, COLUMN_ID",
                        schema);

        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS)
            return result;

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS) {
            dpiStmt_release(stmt);
            return result;
        }

        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            dpiNativeTypeNum nt;
            dpiData* d;

            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            std::string tblName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            std::string colName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 3, &nt, &d);
            std::string dataType = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 4, &nt, &d);
            std::string nullable = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 5, &nt, &d);
            std::string dataLen = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 6, &nt, &d);
            std::string prec = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 7, &nt, &d);
            std::string scale = dpiDataToString(nt, d);

            Column col;
            col.name = colName;
            col.isNotNull = (nullable == "N");

            if (prec != "NULL") {
                if (scale != "NULL" && scale != "0") {
                    col.type = std::format("{}({},{})", dataType, prec, scale);
                } else {
                    col.type = std::format("{}({})", dataType, prec);
                }
            } else if (dataType == "VARCHAR2" || dataType == "CHAR" || dataType == "NVARCHAR2" ||
                       dataType == "RAW") {
                col.type = std::format("{}({})", dataType, dataLen);
            } else {
                col.type = dataType;
            }

            result[tblName].push_back(col);
        }

        dpiStmt_release(stmt);
        return result;
    }

    // batch-load all primary key columns for a schema
    std::unordered_map<std::string, std::vector<std::string>>
    batchLoadPrimaryKeys(dpiConn* conn, const std::string& schema) {
        std::unordered_map<std::string, std::vector<std::string>> result;
        std::string query =
            std::format("SELECT ac.TABLE_NAME, acc.COLUMN_NAME "
                        "FROM ALL_CONS_COLUMNS acc "
                        "JOIN ALL_CONSTRAINTS ac ON acc.CONSTRAINT_NAME = ac.CONSTRAINT_NAME "
                        "AND acc.OWNER = ac.OWNER "
                        "WHERE ac.CONSTRAINT_TYPE = 'P' AND ac.OWNER = '{}'",
                        schema);

        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS)
            return result;

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS) {
            dpiStmt_release(stmt);
            return result;
        }

        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            dpiNativeTypeNum nt;
            dpiData* d;
            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            std::string tblName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            std::string colName = dpiDataToString(nt, d);
            result[tblName].push_back(colName);
        }

        dpiStmt_release(stmt);
        return result;
    }

    // batch-load all foreign keys for a schema
    std::unordered_map<std::string, std::vector<ForeignKey>>
    batchLoadForeignKeys(dpiConn* conn, const std::string& schema) {
        std::unordered_map<std::string, std::vector<ForeignKey>> result;
        std::string query =
            std::format("SELECT ac.TABLE_NAME, ac.CONSTRAINT_NAME, acc.COLUMN_NAME, "
                        "rc.TABLE_NAME, rcc.COLUMN_NAME "
                        "FROM ALL_CONSTRAINTS ac "
                        "JOIN ALL_CONS_COLUMNS acc ON ac.CONSTRAINT_NAME = acc.CONSTRAINT_NAME "
                        "AND ac.OWNER = acc.OWNER "
                        "JOIN ALL_CONSTRAINTS rc ON ac.R_CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                        "AND ac.R_OWNER = rc.OWNER "
                        "JOIN ALL_CONS_COLUMNS rcc ON rc.CONSTRAINT_NAME = rcc.CONSTRAINT_NAME "
                        "AND rc.OWNER = rcc.OWNER AND acc.POSITION = rcc.POSITION "
                        "WHERE ac.CONSTRAINT_TYPE = 'R' AND ac.OWNER = '{}' "
                        "ORDER BY ac.TABLE_NAME, ac.CONSTRAINT_NAME, acc.POSITION",
                        schema);

        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS)
            return result;

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS) {
            dpiStmt_release(stmt);
            return result;
        }

        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            dpiNativeTypeNum nt;
            dpiData* d;
            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            std::string tblName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            std::string fkName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 3, &nt, &d);
            std::string srcCol = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 4, &nt, &d);
            std::string tgtTable = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 5, &nt, &d);
            std::string tgtCol = dpiDataToString(nt, d);

            ForeignKey fk;
            fk.name = fkName;
            fk.sourceColumn = srcCol;
            fk.targetTable = tgtTable;
            fk.targetColumn = tgtCol;
            result[tblName].push_back(fk);
        }

        dpiStmt_release(stmt);
        return result;
    }

    // batch-load all indexes for a schema (excluding PK indexes)
    std::unordered_map<std::string, std::vector<Index>>
    batchLoadIndexes(dpiConn* conn, const std::string& schema) {
        std::unordered_map<std::string, std::vector<Index>> result;
        std::string query =
            std::format("SELECT i.TABLE_NAME, i.INDEX_NAME, i.UNIQUENESS, ic.COLUMN_NAME "
                        "FROM ALL_INDEXES i "
                        "JOIN ALL_IND_COLUMNS ic ON i.INDEX_NAME = ic.INDEX_NAME "
                        "AND i.OWNER = ic.INDEX_OWNER "
                        "LEFT JOIN ALL_CONSTRAINTS c ON i.INDEX_NAME = c.INDEX_NAME "
                        "AND i.OWNER = c.OWNER "
                        "WHERE i.OWNER = '{}' "
                        "AND (c.CONSTRAINT_TYPE IS NULL OR c.CONSTRAINT_TYPE != 'P') "
                        "ORDER BY i.TABLE_NAME, i.INDEX_NAME, ic.COLUMN_POSITION",
                        schema);

        dpiStmt* stmt = nullptr;
        if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()),
                                nullptr, 0, &stmt) != DPI_SUCCESS)
            return result;

        uint32_t numCols = 0;
        if (dpiStmt_execute(stmt, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS) {
            dpiStmt_release(stmt);
            return result;
        }

        // intermediate: table -> indexName -> Index
        std::unordered_map<std::string, std::map<std::string, Index>> tableIndexMap;

        int found = 0;
        uint32_t bufIdx = 0;
        while (dpiStmt_fetch(stmt, &found, &bufIdx) == DPI_SUCCESS && found) {
            dpiNativeTypeNum nt;
            dpiData* d;
            dpiStmt_getQueryValue(stmt, 1, &nt, &d);
            std::string tblName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 2, &nt, &d);
            std::string idxName = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 3, &nt, &d);
            std::string uniqueness = dpiDataToString(nt, d);
            dpiStmt_getQueryValue(stmt, 4, &nt, &d);
            std::string colName = dpiDataToString(nt, d);

            auto& idxMap = tableIndexMap[tblName];
            if (!idxMap.contains(idxName)) {
                Index idx;
                idx.name = idxName;
                idx.isUnique = (uniqueness == "UNIQUE");
                idxMap[idxName] = idx;
            }
            idxMap[idxName].columns.push_back(colName);
        }

        dpiStmt_release(stmt);

        for (auto& [tblName, idxMap] : tableIndexMap) {
            for (auto& idx : idxMap | std::views::values) {
                result[tblName].push_back(std::move(idx));
            }
        }

        return result;
    }

} // namespace

void OracleDatabaseNode::ensureConnectionPool() {
    if (!connectionPool && parentDb) {
        auto nodeInfo = parentDb->getConnectionInfo();
        initializeConnectionPool(nodeInfo);
    }
}

void OracleDatabaseNode::initializeConnectionPool(const DatabaseConnectionInfo& info) {
    if (!parentDb)
        return;

    Logger::debug(std::format("initializeConnectionPool (Oracle) {}:{} schema={}", info.host,
                              info.port, name));
    if (connectionPool)
        return;

    constexpr size_t poolSize = 2;
    std::string schema = name;
    connectionPool = std::make_unique<ConnectionPool<dpiConn*>>(
        poolSize,
        [info, schema]() -> dpiConn* {
            OracleDatabase::initContext();
            return openDpiConnection(info, schema);
        },
        [](dpiConn* conn) { closeDpiConnection(conn); },
        [](dpiConn* conn) -> bool { return dpiConnectionAlive(conn); });
}

ConnectionPool<dpiConn*>::Session OracleDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error(
            "OracleDatabaseNode::getSession: Connection pool not available for schema: " + name);
    }
    return connectionPool->acquire();
}

void OracleDatabaseNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        Logger::info(std::format("Async table loading completed for schema {}. Found {} tables",
                                 name, tables.size()));
        tablesLoaded = true;
    });
}

void OracleDatabaseNode::startTablesLoadAsync(bool forceRefresh) {
    Logger::debug("startTablesLoadAsync for schema: " + name +
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDb)
        return;
    if (tablesLoader.isRunning())
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

std::vector<Table> OracleDatabaseNode::getTablesAsync() {
    std::vector<Table> result;

    if (!tablesLoader.isRunning())
        return result;

    try {
        ensureConnectionPool();
        auto session = getSession();
        dpiConn* conn = session.get();

        auto tableNames =
            dpiQueryStringList(conn, std::format("SELECT TABLE_NAME FROM ALL_TABLES "
                                                 "WHERE OWNER = '{}' ORDER BY TABLE_NAME",
                                                 name));

        Logger::debug(std::format("Found {} tables in schema {}", tableNames.size(), name));

        if (tableNames.empty() || !tablesLoader.isRunning())
            return result;

        // batch-load all metadata for the schema in 4 queries instead of 4*N
        auto allColumns = batchLoadColumns(conn, name);
        if (!tablesLoader.isRunning())
            return result;
        auto allPKs = batchLoadPrimaryKeys(conn, name);
        if (!tablesLoader.isRunning())
            return result;
        auto allFKs = batchLoadForeignKeys(conn, name);
        if (!tablesLoader.isRunning())
            return result;
        auto allIndexes = batchLoadIndexes(conn, name);

        const auto connName = parentDb->getConnectionInfo().name;
        for (const auto& tblName : tableNames) {
            if (!tablesLoader.isRunning())
                break;

            Table table;
            table.name = tblName;
            table.fullName = connName + "." + name + "." + tblName;

            // columns
            if (auto it = allColumns.find(tblName); it != allColumns.end()) {
                table.columns = it->second;
            }

            // mark primary keys
            if (auto it = allPKs.find(tblName); it != allPKs.end()) {
                for (const auto& pkCol : it->second) {
                    for (auto& col : table.columns) {
                        if (col.name == pkCol) {
                            col.isPrimaryKey = true;
                            break;
                        }
                    }
                }
            }

            // foreign keys
            if (auto it = allFKs.find(tblName); it != allFKs.end()) {
                table.foreignKeys = it->second;
            }

            // indexes
            if (auto it = allIndexes.find(tblName); it != allIndexes.end()) {
                table.indexes = it->second;
            }

            result.push_back(std::move(table));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting tables for schema {}: {}", name, e.what()));
        lastTablesError = e.what();
    }

    return result;
}

void OracleDatabaseNode::checkViewsStatusAsync() {
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        Logger::info(std::format("Async view loading completed for schema {}. Found {} views", name,
                                 views.size()));
        viewsLoaded = true;
    });
}

void OracleDatabaseNode::startViewsLoadAsync(bool forceRefresh) {
    Logger::debug("startViewsLoadAsync for schema: " + name);
    if (!parentDb)
        return;

    if (viewsLoader.isRunning() || (viewsLoaded && !forceRefresh))
        return;

    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    viewsLoader.start([this]() { return getViewsForSchemaAsync(); });
}

std::vector<Table> OracleDatabaseNode::getViewsForSchemaAsync() {
    std::vector<Table> result;

    if (!viewsLoader.isRunning())
        return result;

    try {
        ensureConnectionPool();

        if (!viewsLoader.isRunning())
            return result;

        auto session = getSession();
        dpiConn* conn = session.get();

        auto viewNames =
            dpiQueryStringList(conn, std::format("SELECT VIEW_NAME FROM ALL_VIEWS "
                                                 "WHERE OWNER = '{}' ORDER BY VIEW_NAME",
                                                 name));

        Logger::debug(std::format("Found {} views in schema {}", viewNames.size(), name));

        if (viewNames.empty() || !viewsLoader.isRunning())
            return result;

        // reuse batch column loader — columns for views are in ALL_TAB_COLUMNS too
        auto allColumns = batchLoadColumns(conn, name);

        const auto connName = parentDb->getConnectionInfo().name;
        for (const auto& viewName : viewNames) {
            if (!viewsLoader.isRunning())
                break;

            Table view;
            view.name = viewName;
            view.fullName = connName + "." + name + "." + viewName;

            if (auto it = allColumns.find(viewName); it != allColumns.end()) {
                view.columns = it->second;
            }

            result.push_back(std::move(view));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting views for schema {}: {}", name, e.what()));
        lastViewsError = e.what();
    }

    return result;
}

void OracleDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    Logger::debug(std::format("Starting async refresh for table: {}", tableName));

    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning())
        return;

    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

void OracleDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end())
        return;

    it->second.check([this, tableName](const Table& refreshedTable) {
        const auto tableIt = std::ranges::find_if(
            tables, [&tableName](const Table& t) { return t.name == tableName; });

        if (tableIt != tables.end()) {
            *tableIt = refreshedTable;
            Logger::info(std::format("Table {} refreshed successfully", tableName));
        }

        tableRefreshLoaders.erase(tableName);
    });
}

Table OracleDatabaseNode::refreshTableAsync(const std::string& tableName) {
    Logger::debug(std::format("Refreshing table: {}", tableName));

    std::string fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;

    try {
        ensureConnectionPool();
        auto session = getSession();
        dpiConn* conn = session.get();
        return loadTableMetadata(conn, name, tableName, fullName);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error refreshing table {}: {}", tableName, e.what()));
        throw;
    }
}

bool OracleDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

std::vector<std::vector<std::string>>
OracleDatabaseNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                 const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;
    std::string qualified = quoteOracleTable(name, tableName);

    try {
        ensureConnectionPool();
        auto session = getSession();
        dpiConn* conn = session.get();

        std::string query = std::format("SELECT * FROM {}", qualified);

        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        if (!orderByClause.empty()) {
            query += " ORDER BY " + orderByClause;
        } else {
            query += " ORDER BY 1";
        }

        query += std::format(" OFFSET {} ROWS FETCH NEXT {} ROWS ONLY", offset, limit);

        auto qr = dpiExecuteQuery(conn, query, limit);
        if (!qr.statements.empty() && qr.statements[0].success) {
            result = std::move(qr.statements[0].tableData);
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting table data for {}: {}", tableName, e.what()));
    }

    return result;
}

std::vector<std::string> OracleDatabaseNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;

    try {
        ensureConnectionPool();
        auto session = getSession();
        dpiConn* conn = session.get();

        std::string query =
            std::format("SELECT COLUMN_NAME FROM ALL_TAB_COLUMNS "
                        "WHERE OWNER = '{}' AND TABLE_NAME = '{}' ORDER BY COLUMN_ID",
                        name, tableName);

        columnNames = dpiQueryStringList(conn, query);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting column names for {}: {}", tableName, e.what()));
    }

    return columnNames;
}

int OracleDatabaseNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    int count = 0;
    std::string qualified = quoteOracleTable(name, tableName);

    try {
        ensureConnectionPool();
        auto session = getSession();
        dpiConn* conn = session.get();

        std::string query = std::format("SELECT COUNT(*) FROM {}", qualified);
        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        std::string result = dpiQueryScalar(conn, query);
        if (!result.empty() && result != "NULL") {
            count = std::stoi(result);
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting row count for {}: {}", tableName, e.what()));
    }

    return count;
}

QueryResult OracleDatabaseNode::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        ensureConnectionPool();
        auto session = getSession();
        dpiConn* conn = session.get();
        result = dpiExecuteQuery(conn, query, rowLimit);
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

std::pair<bool, std::string> OracleDatabaseNode::createTable(const Table& table) {
    try {
        DDLBuilder builder(getDatabaseType());
        std::string sql = builder.createTable(table);

        auto result = executeQuery(sql);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

std::string OracleDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType OracleDatabaseNode::getDatabaseType() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().type;
    }
    return DatabaseType::ORACLE;
}

void OracleDatabaseNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
}

std::pair<bool, std::string> OracleDatabaseNode::renameTable(const std::string& oldName,
                                                             const std::string& newName) {
    auto sql = std::format("ALTER TABLE \"{}\".\"{}\" RENAME TO \"{}\"", name, oldName, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> OracleDatabaseNode::dropTable(const std::string& tableName) {
    auto sql = std::format("DROP TABLE \"{}\".\"{}\" CASCADE CONSTRAINTS", name, tableName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> OracleDatabaseNode::dropColumn(const std::string& tableName,
                                                            const std::string& columnName) {
    auto sql =
        std::format("ALTER TABLE \"{}\".\"{}\" DROP COLUMN \"{}\"", name, tableName, columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
