#include "database/mssql/mssql_schema_node.hpp"
#include "database/db.hpp"
#include "database/mssql.hpp"
#include "database/mssql/mssql_database_node.hpp"
#include "database/sql_builder.hpp"
#include "mssql_utils.hpp"
#include <algorithm>
#include <format>
#include <spdlog/spdlog.h>

std::string MSSQLSchemaNode::qualifyName(const std::string& tableName) const {
    return quoteMssqlId(name) + "." + quoteMssqlId(tableName);
}

DatabaseInterface* MSSQLSchemaNode::ownerDatabase() const {
    return parentDbNode ? parentDbNode->parentDb : nullptr;
}

std::string MSSQLSchemaNode::getFullPath() const {
    if (parentDbNode)
        return parentDbNode->name + "." + name;
    return name;
}

QueryResult MSSQLSchemaNode::executeQuery(const std::string& query, int rowLimit) {
    if (!parentDbNode)
        return {};
    return parentDbNode->executeQuery(query, rowLimit);
}

std::pair<bool, std::string> MSSQLSchemaNode::createTable(const Table& table) {
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

void MSSQLSchemaNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
}

// tables

void MSSQLSchemaNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        spdlog::debug("Async table loading completed for schema {}.{}. Found {} tables",
                      parentDbNode ? parentDbNode->name : "?", name, tables.size());
        tablesLoaded = true;
        if (parentDbNode)
            parentDbNode->invalidateAggregatedObjects();
    });
}

void MSSQLSchemaNode::startTablesLoadAsync(bool forceRefresh) {
    spdlog::debug("startTablesLoadAsync for schema: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDbNode)
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

std::vector<Table> MSSQLSchemaNode::getTablesAsync() {
    std::vector<Table> result;
    if (!tablesLoader.isRunning())
        return result;

    try {
        parentDbNode->ensureConnectionPool();
        auto session = parentDbNode->getSession();
        DBPROCESS* dbproc = session.get();

        // 1) get table names
        std::vector<std::string> tableNames;
        {
            std::string query =
                std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
                            "WHERE TABLE_TYPE = 'BASE TABLE' AND TABLE_CATALOG = DB_NAME() "
                            "AND TABLE_SCHEMA = '{}' ORDER BY TABLE_NAME",
                            name);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    if (!tablesLoader.isRunning())
                        return result;
                    tableNames.push_back(colToString(dbproc, 1));
                }
            }
            drainResults(dbproc);
        }

        spdlog::debug("Found {} tables in schema {}.{}", tableNames.size(), parentDbNode->name,
                      name);

        if (tableNames.empty() || !tablesLoader.isRunning())
            return result;

        // build IN clause for bulk queries
        std::string inClause;
        for (size_t i = 0; i < tableNames.size(); ++i) {
            if (i > 0)
                inClause += ", ";
            inClause += "'" + tableNames[i] + "'";
        }

        // 2) bulk load all columns
        std::map<std::string, std::vector<Column>> tableColumns;
        {
            std::string query = std::format(
                "SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE, "
                "COLUMNPROPERTY(OBJECT_ID('{0}.' + TABLE_NAME), COLUMN_NAME, 'IsIdentity') "
                "FROM INFORMATION_SCHEMA.COLUMNS "
                "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                "AND TABLE_NAME IN ({1}) ORDER BY TABLE_NAME, ORDINAL_POSITION",
                name, inClause);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    if (!tablesLoader.isRunning())
                        return result;
                    std::string tbl = colToString(dbproc, 1);
                    Column col;
                    col.name = colToString(dbproc, 2);
                    col.type = colToString(dbproc, 3);
                    col.isNotNull = colToString(dbproc, 4) == "NO";
                    tableColumns[tbl].push_back(col);
                }
            }
            drainResults(dbproc);
        }

        if (!tablesLoader.isRunning())
            return result;

        // 3) bulk load all primary keys
        {
            std::string query =
                std::format("SELECT tc.TABLE_NAME, c.COLUMN_NAME "
                            "FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                            "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                            "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                            "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                            "WHERE tc.TABLE_SCHEMA = '{0}' AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY' "
                            "AND tc.TABLE_NAME IN ({1})",
                            name, inClause);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    std::string tbl = colToString(dbproc, 1);
                    std::string pkCol = colToString(dbproc, 2);
                    auto it = tableColumns.find(tbl);
                    if (it != tableColumns.end()) {
                        for (auto& col : it->second) {
                            if (col.name == pkCol) {
                                col.isPrimaryKey = true;
                                break;
                            }
                        }
                    }
                }
            }
            drainResults(dbproc);
        }

        if (!tablesLoader.isRunning())
            return result;

        // 4) bulk load all foreign keys
        std::map<std::string, std::vector<ForeignKey>> tableForeignKeys;
        {
            std::string query = std::format(
                "SELECT OBJECT_NAME(fk.parent_object_id), fk.name, "
                "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
                "OBJECT_NAME(fkc.referenced_object_id), "
                "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
                "FROM sys.foreign_keys fk "
                "JOIN sys.foreign_key_columns fkc ON fk.object_id = fkc.constraint_object_id "
                "WHERE OBJECT_SCHEMA_NAME(fk.parent_object_id) = '{}'",
                name);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    std::string tbl = colToString(dbproc, 1);
                    ForeignKey fk;
                    fk.name = colToString(dbproc, 2);
                    fk.sourceColumn = colToString(dbproc, 3);
                    fk.targetTable = colToString(dbproc, 4);
                    fk.targetColumn = colToString(dbproc, 5);
                    tableForeignKeys[tbl].push_back(fk);
                }
            }
            drainResults(dbproc);
        }

        if (!tablesLoader.isRunning())
            return result;

        // 5) bulk load all indexes
        std::map<std::string, std::vector<Index>> tableIndexes;
        {
            std::string query =
                std::format("SELECT OBJECT_NAME(i.object_id), i.name, i.is_unique, c.name "
                            "FROM sys.indexes i "
                            "JOIN sys.index_columns ic ON i.object_id = ic.object_id "
                            "AND i.index_id = ic.index_id "
                            "JOIN sys.columns c ON ic.object_id = c.object_id "
                            "AND ic.column_id = c.column_id "
                            "WHERE i.is_primary_key = 0 AND i.type > 0 "
                            "AND OBJECT_SCHEMA_NAME(i.object_id) = '{}' "
                            "ORDER BY OBJECT_NAME(i.object_id), i.name, ic.key_ordinal",
                            name);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    std::string tbl = colToString(dbproc, 1);
                    std::string idxName = colToString(dbproc, 2);
                    int isUnique = colToInt(dbproc, 3);
                    std::string colName = colToString(dbproc, 4);

                    auto& idxVec = tableIndexes[tbl];
                    if (idxVec.empty() || idxVec.back().name != idxName) {
                        Index idx;
                        idx.name = idxName;
                        idx.isUnique = (isUnique != 0);
                        idx.columns.push_back(colName);
                        idxVec.push_back(std::move(idx));
                    } else {
                        idxVec.back().columns.push_back(colName);
                    }
                }
            }
            drainResults(dbproc);
        }

        // 6) assemble Table objects
        const auto connName =
            parentDbNode->parentDb ? parentDbNode->parentDb->getConnectionInfo().name : "";
        for (const auto& tblName : tableNames) {
            if (!tablesLoader.isRunning())
                break;

            Table table;
            table.name = tblName;
            table.fullName = connName + "." + parentDbNode->name + "." + name + "." + tblName;

            auto colIt = tableColumns.find(tblName);
            if (colIt != tableColumns.end())
                table.columns = std::move(colIt->second);

            auto fkIt = tableForeignKeys.find(tblName);
            if (fkIt != tableForeignKeys.end())
                table.foreignKeys = std::move(fkIt->second);

            auto idxIt = tableIndexes.find(tblName);
            if (idxIt != tableIndexes.end())
                table.indexes = std::move(idxIt->second);

            result.push_back(std::move(table));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting tables for schema {}.{}: {}", parentDbNode->name, name,
                      e.what());
        lastTablesError = e.what();
    }

    return result;
}

// views

void MSSQLSchemaNode::checkViewsStatusAsync() {
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        spdlog::debug("Async view loading completed for schema {}.{}. Found {} views",
                      parentDbNode ? parentDbNode->name : "?", name, views.size());
        viewsLoaded = true;
        if (parentDbNode)
            parentDbNode->invalidateAggregatedObjects();
    });
}

void MSSQLSchemaNode::startViewsLoadAsync(bool forceRefresh) {
    spdlog::debug("startViewsLoadAsync for schema: {}", name);
    if (!parentDbNode)
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

std::vector<Table> MSSQLSchemaNode::getViewsAsync() {
    std::vector<Table> result;
    if (!viewsLoader.isRunning())
        return result;

    try {
        parentDbNode->ensureConnectionPool();
        auto session = parentDbNode->getSession();
        DBPROCESS* dbproc = session.get();

        std::vector<std::string> viewNames;
        {
            std::string query =
                std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS "
                            "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                            "ORDER BY TABLE_NAME",
                            name);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    if (!viewsLoader.isRunning())
                        return result;
                    viewNames.push_back(colToString(dbproc, 1));
                }
            }
            drainResults(dbproc);
        }

        spdlog::debug("Found {} views in schema {}.{}", viewNames.size(), parentDbNode->name, name);

        if (viewNames.empty() || !viewsLoader.isRunning())
            return result;

        // bulk load columns for all views
        std::string inClause;
        for (size_t i = 0; i < viewNames.size(); ++i) {
            if (i > 0)
                inClause += ", ";
            inClause += "'" + viewNames[i] + "'";
        }

        std::map<std::string, std::vector<Column>> viewColumns;
        {
            std::string query =
                std::format("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE "
                            "FROM INFORMATION_SCHEMA.COLUMNS "
                            "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                            "AND TABLE_NAME IN ({1}) ORDER BY TABLE_NAME, ORDINAL_POSITION",
                            name, inClause);

            if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
                while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                    if (!viewsLoader.isRunning())
                        return result;
                    std::string tbl = colToString(dbproc, 1);
                    Column col;
                    col.name = colToString(dbproc, 2);
                    col.type = colToString(dbproc, 3);
                    col.isNotNull = colToString(dbproc, 4) == "NO";
                    viewColumns[tbl].push_back(col);
                }
            }
            drainResults(dbproc);
        }

        const auto connName =
            parentDbNode->parentDb ? parentDbNode->parentDb->getConnectionInfo().name : "";
        for (const auto& vName : viewNames) {
            if (!viewsLoader.isRunning())
                break;

            Table view;
            view.name = vName;
            view.fullName = connName + "." + parentDbNode->name + "." + name + "." + vName;
            auto it = viewColumns.find(vName);
            if (it != viewColumns.end())
                view.columns = std::move(it->second);
            result.push_back(std::move(view));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting views for schema {}.{}: {}", parentDbNode->name, name,
                      e.what());
        lastViewsError = e.what();
    }

    return result;
}

// table refresh

void MSSQLSchemaNode::startTableRefreshAsync(const std::string& tableName) {
    spdlog::debug("Starting async refresh for table: {}.{}", name, tableName);

    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning())
        return;

    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

void MSSQLSchemaNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end())
        return;

    it->second.check([this, tableName](const Table& refreshedTable) {
        const auto tableIt = std::ranges::find_if(
            tables, [&tableName](const Table& t) { return t.name == tableName; });

        if (tableIt != tables.end()) {
            *tableIt = refreshedTable;
            spdlog::debug("Table {}.{} refreshed successfully", name, tableName);
        }

        tableRefreshLoaders.erase(tableName);
    });
}

bool MSSQLSchemaNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

Table MSSQLSchemaNode::refreshTableAsync(const std::string& tableName) {
    spdlog::debug("Refreshing table: {}.{}", name, tableName);

    const auto connName = parentDbNode && parentDbNode->parentDb
                              ? parentDbNode->parentDb->getConnectionInfo().name
                              : "";
    std::string fullName = connName + "." + parentDbNode->name + "." + name + "." + tableName;

    try {
        auto session = parentDbNode->getSession();
        DBPROCESS* dbproc = session.get();
        return loadTableMetadata(dbproc, name, tableName, tableName, fullName);
    } catch (const std::exception& e) {
        spdlog::error("Error refreshing table {}.{}: {}", name, tableName, e.what());
        throw;
    }
}

// data access

std::vector<std::vector<std::string>>
MSSQLSchemaNode::getTableData(const std::string& tableName, const int limit, const int offset,
                              const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;
    std::string qualified = qualifyName(tableName);

    try {
        auto session = parentDbNode->getSession();
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
            spdlog::error("Error getting table data for {}.{}: {}", name, tableName,
                          getLastError());
        }
        drainResults(dbproc);
    } catch (const std::exception& e) {
        spdlog::error("Error getting table data for {}.{}: {}", name, tableName, e.what());
    }

    return result;
}

std::vector<std::string> MSSQLSchemaNode::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;

    try {
        auto session = parentDbNode->getSession();
        DBPROCESS* dbproc = session.get();

        std::string query = std::format("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
                                        "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                                        "AND TABLE_NAME = '{}' ORDER BY ORDINAL_POSITION",
                                        name, tableName);

        if (execQuery(dbproc, query) && dbresults(dbproc) == SUCCEED) {
            while (dbnextrow(dbproc) != NO_MORE_ROWS) {
                columnNames.push_back(colToString(dbproc, 1));
            }
        }
        drainResults(dbproc);
    } catch (const std::exception& e) {
        spdlog::error("Error getting column names for {}.{}: {}", name, tableName, e.what());
    }

    return columnNames;
}

int MSSQLSchemaNode::getRowCount(const std::string& tableName, const std::string& whereClause) {
    int count = 0;
    std::string qualified = qualifyName(tableName);

    try {
        auto session = parentDbNode->getSession();
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
        spdlog::error("Error getting row count for {}.{}: {}", name, tableName, e.what());
    }

    return count;
}

// schema modification

std::pair<bool, std::string> MSSQLSchemaNode::renameTable(const std::string& oldName,
                                                          const std::string& newName) {
    auto sql = std::format("EXEC sp_rename '{}.{}', '{}'", name, oldName, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MSSQLSchemaNode::dropTable(const std::string& tableName) {
    auto sql = std::format("DROP TABLE {}", qualifyName(tableName));
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MSSQLSchemaNode::truncateTable(const std::string& tableName) {
    auto sql = std::format("TRUNCATE TABLE {}", qualifyName(tableName));
    auto r = executeQuery(sql);
    if (r.success())
        return {true, ""};
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MSSQLSchemaNode::dropColumn(const std::string& tableName,
                                                         const std::string& columnName) {
    auto sql = std::format("ALTER TABLE {} DROP COLUMN [{}]", qualifyName(tableName), columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
