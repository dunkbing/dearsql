#include "database/mssql/mssql_database_node.hpp"
#include "database/db.hpp"
#include "database/ddl_builder.hpp"
#include "database/mssql.hpp"
#include "mssql_utils.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <ranges>

namespace {

    // load columns for a table/view
    std::vector<Column> loadColumns(DBPROCESS* dbproc, const std::string& schema,
                                    const std::string& tableName) {
        std::vector<Column> columns;
        std::string query =
            std::format("SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, "
                        "COLUMNPROPERTY(OBJECT_ID('{}.{}'), COLUMN_NAME, 'IsIdentity') "
                        "FROM INFORMATION_SCHEMA.COLUMNS "
                        "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                        "AND TABLE_NAME = '{}' ORDER BY ORDINAL_POSITION",
                        schema, tableName, schema, tableName);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                Column col;
                col.name = colToString(dbproc, 1);
                col.type = colToString(dbproc, 2);
                col.isNotNull = colToString(dbproc, 3) == "NO";
                columns.push_back(col);
            }
        }
        drainResults(dbproc);
        return columns;
    }

    // mark primary key columns
    void loadPrimaryKeys(DBPROCESS* dbproc, const std::string& schema, const std::string& tableName,
                         std::vector<Column>& columns) {
        std::string query = std::format("SELECT c.COLUMN_NAME "
                                        "FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                                        "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                                        "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                                        "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                                        "WHERE tc.TABLE_SCHEMA = '{}' AND tc.TABLE_NAME = '{}' "
                                        "AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY'",
                                        schema, tableName);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                std::string pkCol = colToString(dbproc, 1);
                for (auto& col : columns) {
                    if (col.name == pkCol) {
                        col.isPrimaryKey = true;
                        break;
                    }
                }
            }
        }
        drainResults(dbproc);
    }

    // load foreign keys for a table
    std::vector<ForeignKey> loadForeignKeys(DBPROCESS* dbproc, const std::string& schema,
                                            const std::string& tableName) {
        std::vector<ForeignKey> fks;
        std::string query = std::format(
            "SELECT fk.name, "
            "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
            "OBJECT_NAME(fkc.referenced_object_id), "
            "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
            "FROM sys.foreign_keys fk "
            "JOIN sys.foreign_key_columns fkc ON fk.object_id = fkc.constraint_object_id "
            "WHERE OBJECT_SCHEMA_NAME(fk.parent_object_id) = '{}' "
            "AND OBJECT_NAME(fk.parent_object_id) = '{}'",
            schema, tableName);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                ForeignKey fk;
                fk.name = colToString(dbproc, 1);
                fk.sourceColumn = colToString(dbproc, 2);
                fk.targetTable = colToString(dbproc, 3);
                fk.targetColumn = colToString(dbproc, 4);
                fks.push_back(fk);
            }
        }
        drainResults(dbproc);
        return fks;
    }

    // load indexes for a table
    std::vector<Index> loadIndexes(DBPROCESS* dbproc, const std::string& schema,
                                   const std::string& tableName) {
        std::vector<Index> indexes;
        std::string query = std::format("SELECT i.name, i.is_unique, c.name AS column_name "
                                        "FROM sys.indexes i "
                                        "JOIN sys.index_columns ic ON i.object_id = ic.object_id "
                                        "AND i.index_id = ic.index_id "
                                        "JOIN sys.columns c ON ic.object_id = c.object_id "
                                        "AND ic.column_id = c.column_id "
                                        "WHERE i.object_id = OBJECT_ID('{}.{}') "
                                        "AND i.is_primary_key = 0 AND i.type > 0 "
                                        "ORDER BY i.name, ic.key_ordinal",
                                        schema, tableName);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            std::map<std::string, Index> indexMap;
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                std::string idxName = colToString(dbproc, 1);
                int isUnique = colToInt(dbproc, 2);
                std::string colName = colToString(dbproc, 3);

                if (!indexMap.contains(idxName)) {
                    Index idx;
                    idx.name = idxName;
                    idx.isUnique = (isUnique != 0);
                    indexMap[idxName] = idx;
                }
                indexMap[idxName].columns.push_back(colName);
            }
            for (auto& idx : indexMap | std::views::values) {
                indexes.push_back(std::move(idx));
            }
        }
        drainResults(dbproc);
        return indexes;
    }

    // load all metadata for a single table
    Table loadTableMetadata(DBPROCESS* dbproc, const std::string& schema,
                            const std::string& tableName, const std::string& displayName,
                            const std::string& fullName) {
        Table table;
        table.name = displayName;
        table.fullName = fullName;
        table.columns = loadColumns(dbproc, schema, tableName);
        loadPrimaryKeys(dbproc, schema, tableName, table.columns);
        table.foreignKeys = loadForeignKeys(dbproc, schema, tableName);
        table.indexes = loadIndexes(dbproc, schema, tableName);
        return table;
    }

} // namespace

void MSSQLDatabaseNode::ensureConnectionPool() {
    if (!connectionPool && parentDb) {
        auto nodeInfo = parentDb->getConnectionInfo();
        nodeInfo.database = name;
        initializeConnectionPool(nodeInfo);
    }
}

void MSSQLDatabaseNode::initializeConnectionPool(const DatabaseConnectionInfo& info) {
    if (!parentDb)
        return;

    Logger::debug(std::format("initializeConnectionPool (MSSQL) {}:{} db={}", info.host, info.port,
                              info.database));
    if (connectionPool)
        return;

    constexpr size_t poolSize = 2;
    connectionPool = std::make_unique<ConnectionPool<DBPROCESS*>>(
        poolSize,
        [info]() -> DBPROCESS* {
            MSSQLDatabase::initDbLib();
            return openDbLibConnection(info, info.database);
        },
        [](DBPROCESS* dbproc) { dbclose(dbproc); },
        [](DBPROCESS* dbproc) -> bool { return !dbdead(dbproc); });
}

ConnectionPool<DBPROCESS*>::Session MSSQLDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error(
            "MSSQLDatabaseNode::getSession: Connection pool not available for database: " + name);
    }
    return connectionPool->acquire();
}

void MSSQLDatabaseNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        Logger::info(std::format("Async table loading completed for database {}. Found {} tables",
                                 name, tables.size()));
        tablesLoaded = true;
    });
}

void MSSQLDatabaseNode::startTablesLoadAsync(bool forceRefresh) {
    Logger::debug("startTablesLoadAsync for db: " + name +
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

std::vector<Table> MSSQLDatabaseNode::getTablesAsync() {
    std::vector<Table> result;

    if (!tablesLoader.isRunning())
        return result;

    try {
        ensureConnectionPool();
        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        // get table names with schema
        struct TableInfo {
            std::string schema;
            std::string name;
        };
        std::vector<TableInfo> tableInfos;

        {
            const char* query = "SELECT TABLE_SCHEMA, TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
                                "WHERE TABLE_TYPE = 'BASE TABLE' AND TABLE_CATALOG = DB_NAME() "
                                "ORDER BY TABLE_SCHEMA, TABLE_NAME";

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    if (!tablesLoader.isRunning())
                        return result;
                    tableInfos.push_back({colToString(dbproc, 1), colToString(dbproc, 2)});
                }
            }
            drainResults(dbproc);
        }

        Logger::debug(std::format("Found {} tables in database {}", tableInfos.size(), name));

        if (tableInfos.empty() || !tablesLoader.isRunning())
            return result;

        const auto connName = parentDb->getConnectionInfo().name;
        for (const auto& ti : tableInfos) {
            if (!tablesLoader.isRunning())
                break;

            std::string displayName = (ti.schema == "dbo") ? ti.name : (ti.schema + "." + ti.name);
            std::string fullName = connName + "." + name + "." + displayName;

            result.push_back(loadTableMetadata(dbproc, ti.schema, ti.name, displayName, fullName));
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting tables for database {}: {}", name, e.what()));
        lastTablesError = e.what();
    }

    return result;
}

void MSSQLDatabaseNode::checkViewsStatusAsync() {
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        Logger::info(std::format("Async view loading completed for database {}. Found {} views",
                                 name, views.size()));
        viewsLoaded = true;
    });
}

void MSSQLDatabaseNode::startViewsLoadAsync(bool forceRefresh) {
    Logger::debug("startViewsLoadAsync for database: " + name);
    if (!parentDb)
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

std::vector<Table> MSSQLDatabaseNode::getViewsForDatabaseAsync() {
    std::vector<Table> result;

    if (!viewsLoader.isRunning())
        return result;

    try {
        ensureConnectionPool();

        if (!viewsLoader.isRunning())
            return result;

        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        struct ViewInfo {
            std::string schema;
            std::string name;
        };
        std::vector<ViewInfo> viewInfos;

        {
            const char* query = "SELECT TABLE_SCHEMA, TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS "
                                "WHERE TABLE_CATALOG = DB_NAME() "
                                "ORDER BY TABLE_SCHEMA, TABLE_NAME";

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    if (!viewsLoader.isRunning())
                        return result;
                    viewInfos.push_back({colToString(dbproc, 1), colToString(dbproc, 2)});
                }
            }
            drainResults(dbproc);
        }

        Logger::debug(std::format("Found {} views in database {}", viewInfos.size(), name));

        const auto connName = parentDb->getConnectionInfo().name;
        for (const auto& vi : viewInfos) {
            if (!viewsLoader.isRunning())
                break;

            std::string displayName = (vi.schema == "dbo") ? vi.name : (vi.schema + "." + vi.name);
            std::string fullName = connName + "." + name + "." + displayName;

            // views only get columns (no PKs, FKs, indexes)
            Table view;
            view.name = displayName;
            view.fullName = fullName;
            view.columns = loadColumns(dbproc, vi.schema, vi.name);
            result.push_back(view);
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting views for database {}: {}", name, e.what()));
        lastViewsError = e.what();
    }

    return result;
}

void MSSQLDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    Logger::debug(std::format("Starting async refresh for table: {}", tableName));

    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning())
        return;

    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

void MSSQLDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
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

Table MSSQLDatabaseNode::refreshTableAsync(const std::string& tableName) {
    Logger::debug(std::format("Refreshing table: {}", tableName));

    auto [schema, tblName] = splitSchemaTable(tableName);
    std::string fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;

    try {
        auto session = getSession();
        DBPROCESS* dbproc = session.get();
        return loadTableMetadata(dbproc, schema, tblName, tableName, fullName);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error refreshing table {}: {}", tableName, e.what()));
        throw;
    }
}

bool MSSQLDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

std::vector<std::vector<std::string>>
MSSQLDatabaseNode::getTableData(const std::string& tableName, const int limit, const int offset,
                                const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;
    std::string qualified = quoteTableName(tableName);

    try {
        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        std::string query = std::format("SELECT * FROM {}", qualified);

        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        if (!orderByClause.empty()) {
            query += " ORDER BY " + orderByClause;
        } else {
            query += " ORDER BY (SELECT NULL)";
        }

        query += std::format(" OFFSET {} ROWS FETCH NEXT {} ROWS ONLY", offset, limit);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            int numCols = dbnumcols(dbproc);
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                std::vector<std::string> rowData;
                rowData.reserve(numCols);
                for (int i = 1; i <= numCols; i++) {
                    rowData.push_back(colToString(dbproc, i));
                }
                result.push_back(std::move(rowData));
            }
        } else {
            Logger::error(
                std::format("Error getting table data for {}: {}", tableName, getLastError()));
        }
        drainResults(dbproc);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting table data for {}: {}", tableName, e.what()));
    }

    return result;
}

std::vector<std::string> MSSQLDatabaseNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;
    auto [schema, tblName] = splitSchemaTable(tableName);

    try {
        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        std::string query = std::format("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
                                        "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                                        "AND TABLE_NAME = '{}' ORDER BY ORDINAL_POSITION",
                                        schema, tblName);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                columnNames.push_back(colToString(dbproc, 1));
            }
        }
        drainResults(dbproc);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting column names for {}: {}", tableName, e.what()));
    }

    return columnNames;
}

int MSSQLDatabaseNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    int count = 0;
    std::string qualified = quoteTableName(tableName);

    try {
        auto session = getSession();
        DBPROCESS* dbproc = session.get();

        std::string query = std::format("SELECT COUNT(*) FROM {}", qualified);
        if (!whereClause.empty())
            query += " WHERE " + whereClause;

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            if (dbnextrow(dbproc) != NO_MORE_ROWS) {
                count = colToInt(dbproc, 1);
            }
        }
        drainResults(dbproc);
    } catch (const std::exception& e) {
        Logger::error(std::format("Error getting row count for {}: {}", tableName, e.what()));
    }

    return count;
}

QueryResult MSSQLDatabaseNode::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        auto session = getSession();
        result = executeQueryOnProcess(session.get(), query, rowLimit);
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

std::pair<bool, std::string> MSSQLDatabaseNode::createTable(const Table& table) {
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

std::string MSSQLDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType MSSQLDatabaseNode::getDatabaseType() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().type;
    }
    return DatabaseType::MSSQL;
}

void MSSQLDatabaseNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
}

std::pair<bool, std::string> MSSQLDatabaseNode::renameTable(const std::string& oldName,
                                                            const std::string& newName) {
    auto sql = std::format("EXEC sp_rename '{}', '{}'", oldName, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MSSQLDatabaseNode::dropTable(const std::string& tableName) {
    auto sql = std::format("DROP TABLE {}", quoteTableName(tableName));
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MSSQLDatabaseNode::dropColumn(const std::string& tableName,
                                                           const std::string& columnName) {
    auto sql =
        std::format("ALTER TABLE {} DROP COLUMN [{}]", quoteTableName(tableName), columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
