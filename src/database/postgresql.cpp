#include "database/postgresql.hpp"
#include "database/db.hpp"
#include "ui/log_panel.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <vector>

PostgresDatabase::PostgresDatabase(const std::string& name, const std::string& host, const int port,
                                   const std::string& database, const std::string& username,
                                   const std::string& password, const bool showAllDatabases)
    : name(name), host(host), port(port), database(database), username(username),
      password(password), showAllDatabases(showAllDatabases) {
    connectionString = buildConnectionString(database);
    if (database.empty()) {
        this->database = "postgres";
    }
}

PostgresDatabase::~PostgresDatabase() {
    // Stop all async operations before cleaning up
    connecting = false;
    loadingDatabases = false;
    switchingDatabase = false;

    tableDataLoader.cancelAllAndWait();

    // Stop all per-database async operations
    for (auto& dbData : databaseDataCache | std::views::values) {
        dbData.loadingTables = false;
        dbData.loadingViews = false;
        dbData.loadingSequences = false;
        dbData.loadingSchemas = false;

        // Wait for all futures to complete
        if (dbData.tablesFuture.valid()) {
            dbData.tablesFuture.wait();
        }
        if (dbData.viewsFuture.valid()) {
            dbData.viewsFuture.wait();
        }
        if (dbData.sequencesFuture.valid()) {
            dbData.sequencesFuture.wait();
        }
        if (dbData.schemasFuture.valid()) {
            dbData.schemasFuture.wait();
        }
    }

    if (connectionFuture.valid()) {
        connectionFuture.wait();
    }
    if (databasesFuture.valid()) {
        databasesFuture.wait();
    }
    if (databaseSwitchFuture.valid()) {
        databaseSwitchFuture.wait();
    }

    // Wait for all per-database schema futures to complete
    for (auto& future : databaseSchemaFutures | std::views::values) {
        if (future.valid()) {
            future.wait();
        }
    }

    PostgresDatabase::disconnect();
}

// Helper methods for per-database data access
PostgresDatabase::DatabaseData& PostgresDatabase::getCurrentDatabaseData() {
    return databaseDataCache[database];
}

const PostgresDatabase::DatabaseData& PostgresDatabase::getCurrentDatabaseData() const {
    static const DatabaseData emptyData;
    const auto it = databaseDataCache.find(database);
    return (it != databaseDataCache.end()) ? it->second : emptyData;
}

PostgresDatabase::DatabaseData& PostgresDatabase::getDatabaseData(const std::string& dbName) {
    return databaseDataCache[dbName];
}

const PostgresDatabase::DatabaseData&
PostgresDatabase::getDatabaseData(const std::string& dbName) const {
    static const DatabaseData emptyData;
    const auto it = databaseDataCache.find(dbName);
    return (it != databaseDataCache.end()) ? it->second : emptyData;
}

std::pair<bool, std::string> PostgresDatabase::connect() {
    const auto* pool = getConnectionPoolForDatabase(database);
    if (connected && pool) {
        return {true, ""};
    }

    attemptedConnection = true;

    try {
        initializeConnectionPool(database, connectionString);
        LogPanel::info("Successfully connected to PostgreSQL database: " + database);
        connected = true;
        lastConnectionError.clear();

        // Start loading databases immediately if showAllDatabases is enabled
        if (showAllDatabases && !databasesLoaded && !loadingDatabases.load()) {
            LogPanel::debug("Starting async database loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::cerr << "Connection to database failed: " << e.what() << std::endl;
        std::lock_guard lock(sessionMutex);
        connectionPools.erase(database);
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    }
}

void PostgresDatabase::disconnect() {
    std::lock_guard lock(sessionMutex);
    connectionPools.clear();
    connected = false;
}

bool PostgresDatabase::isConnected() const {
    return connected && getConnectionPoolForDatabase(database) != nullptr;
}

const std::string& PostgresDatabase::getName() const {
    return name;
}

const std::string& PostgresDatabase::getConnectionString() const {
    return connectionString;
}

const std::string& PostgresDatabase::getPath() const {
    return connectionString;
}

DatabaseType PostgresDatabase::getType() const {
    return DatabaseType::POSTGRESQL;
}

const std::string& PostgresDatabase::getDatabaseName() const {
    return database;
}

void PostgresDatabase::refreshTables() {
    LogPanel::debug("Refreshing tables for database: " + name);
    if (!isConnected()) {
        LogPanel::warn("Not connected to database, cannot refresh tables");
        getCurrentDatabaseData().tablesLoaded = true;
        return;
    }

    startRefreshTableAsync();
}

const std::vector<Table>& PostgresDatabase::getTables() const {
    return getCurrentDatabaseData().tables;
}

std::vector<Table>& PostgresDatabase::getTables() {
    return getCurrentDatabaseData().tables;
}

bool PostgresDatabase::areTablesLoaded() const {
    return getCurrentDatabaseData().tablesLoaded;
}

void PostgresDatabase::setTablesLoaded(const bool loaded) {
    getCurrentDatabaseData().tablesLoaded = loaded;
}

std::string PostgresDatabase::executeQuery(const std::string& query) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            return "Error: Failed to connect to database: " + error;
        }
    }

    try {
        auto sql = getSession();
        std::stringstream output;

        // Check if this is a DDL statement (ALTER, CREATE, DROP, etc.)
        std::string upperQuery = query;
        std::ranges::transform(upperQuery, upperQuery.begin(), ::toupper);
        const bool isDDL = upperQuery.find("ALTER ") == 0 || upperQuery.find("CREATE ") == 0 ||
                           upperQuery.find("DROP ") == 0 || upperQuery.find("COMMENT ") == 0;

        if (isDDL) {
            // Use once() for DDL statements
            *sql << query;
            output << "Query executed successfully.";
        } else {
            // Use prepare for SELECT and other DML statements
            const soci::rowset rs = sql->prepare << query;

            // Get column names if available
            const auto it = rs.begin();
            if (it != rs.end()) {
                const soci::row& firstRow = *it;
                for (std::size_t i = 0; i < firstRow.size(); ++i) {
                    output << firstRow.get_properties(i).get_name();
                    if (i < firstRow.size() - 1)
                        output << " | ";
                }
                output << "\n";

                for (std::size_t i = 0; i < firstRow.size(); ++i) {
                    output << "----------";
                    if (i < firstRow.size() - 1)
                        output << "-+-";
                }
                output << "\n";
            }

            int rowCount = 0;
            for (const auto& row : rs) {
                if (rowCount >= 1000)
                    break;
                for (std::size_t i = 0; i < row.size(); ++i) {
                    output << convertRowValue(row, i);
                    if (i < row.size() - 1)
                        output << " | ";
                }
                output << "\n";
                rowCount++;
            }

            if (rowCount == 0) {
                output << "Query executed successfully.";
            } else if (rowCount == 1000) {
                output << "\n... (showing first 1000 rows)";
            }
        }

        return output.str();
    } catch (const soci::soci_error& e) {
        return "Error: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
PostgresDatabase::executeQueryStructured(const std::string& query) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            return {columnNames, data};
        }
    }

    try {
        const auto sql = getSession();
        const soci::rowset rs = sql->prepare << query;

        // Get column names if available
        const auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row& firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        int rowCount = 0;
        for (const auto& row : rs) {
            if (rowCount >= 1000)
                break;

            std::vector<std::string> rowData;
            rowData.reserve(row.size());
            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.emplace_back(convertRowValue(row, i));
            }
            data.push_back(rowData);
            rowCount++;
        }

        return {columnNames, data};
    } catch (const soci::soci_error& e) {
        LogPanel::error("[soci] Postgres Error: " + std::string(e.what()));
        return {columnNames, data};
    }
}

std::vector<std::vector<std::string>>
PostgresDatabase::getTableData(const std::string& tableName, const int limit, const int offset) {
    std::vector<std::vector<std::string>> data;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return data;
        }
    }

    try {
        const auto session = getSession();
        const std::string sqlQuery = "SELECT * FROM \"" + tableName + "\" LIMIT " +
                                     std::to_string(limit) + " OFFSET " + std::to_string(offset);

        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            std::vector<std::string> rowData;
            rowData.reserve(row.size());

            for (std::size_t i = 0; i < row.size(); ++i) {
                rowData.emplace_back(convertRowValue(row, i));
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> PostgresDatabase::getColumnNames(const std::string& tableName) {
    std::vector<std::string> columnNames;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return columnNames;
        }
    }

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            std::format("SELECT column_name FROM information_schema.columns "
                        "WHERE table_name = '{}' ORDER BY ordinal_position",
                        tableName);

        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            columnNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }

    return columnNames;
}

int PostgresDatabase::getRowCount(const std::string& tableName) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return 0;
        }
    }

    try {
        const auto session = getSession();
        const std::string sqlQuery = std::format(R"(SELECT COUNT(*) FROM "{}")", tableName);
        int count = 0;
        *session << sqlQuery, soci::into(count);
        return count;
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

bool PostgresDatabase::isExpanded() const {
    return expanded;
}

void PostgresDatabase::setExpanded(bool exp) {
    expanded = exp;
}

bool PostgresDatabase::hasAttemptedConnection() const {
    return attemptedConnection;
}

void PostgresDatabase::setAttemptedConnection(const bool attempted) {
    attemptedConnection = attempted;
}

const std::string& PostgresDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void PostgresDatabase::setLastConnectionError(const std::string& error) {
    lastConnectionError = error;
}

void* PostgresDatabase::getConnection() const {
    return getConnectionPoolForDatabase(database);
}

std::vector<std::string> PostgresDatabase::getTableNames() {
    std::vector<std::string> tableNames;

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename";

        LogPanel::debug("Executing query to get table names...");
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            auto tableName = row.get<std::string>(0);
            LogPanel::debug("Found table: " + tableName);
            tableNames.push_back(tableName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    LogPanel::debug("Query completed. Found " + std::to_string(tableNames.size()) + " tables.");
    return tableNames;
}

std::vector<Column> PostgresDatabase::getTableColumns(const std::string& tableName) {
    std::vector<Column> columns;

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            "SELECT c.column_name, c.data_type, c.is_nullable, "
            "CASE WHEN tc.constraint_type = 'PRIMARY KEY' THEN 'true' ELSE 'false' END "
            "as is_primary_key "
            "FROM information_schema.columns c "
            "LEFT JOIN information_schema.key_column_usage kcu ON c.column_name = "
            "kcu.column_name AND c.table_name = kcu.table_name "
            "LEFT JOIN information_schema.table_constraints tc ON "
            "kcu.constraint_name = tc.constraint_name "
            "WHERE c.table_name = '" +
            tableName +
            "' "
            "ORDER BY c.ordinal_position";

        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            Column col;
            col.name = row.get<std::string>(0);
            col.type = row.get<std::string>(1);
            col.isNotNull = row.get<std::string>(2) == "NO";
            auto isPkStr = row.get<std::string>(3);
            col.isPrimaryKey = (isPkStr == "true");
            columns.push_back(col);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table columns: " << e.what() << std::endl;
    }

    return columns;
}

std::vector<Index> PostgresDatabase::getTableIndexes(const std::string& tableName) {
    std::vector<Index> indexes;

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            std::format("SELECT "
                        "    i.relname as index_name, "
                        "    ix.indisunique as is_unique, "
                        "    ix.indisprimary as is_primary, "
                        "    am.amname as index_type, "
                        "    array_to_string(array_agg(a.attname ORDER BY "
                        "array_position(ix.indkey, a.attnum)), ',') as column_names "
                        "FROM pg_index ix "
                        "JOIN pg_class t ON t.oid = ix.indrelid "
                        "JOIN pg_class i ON i.oid = ix.indexrelid "
                        "JOIN pg_am am ON i.relam = am.oid "
                        "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = ANY(ix.indkey) "
                        "WHERE t.relname = '{}' "
                        "AND t.relkind = 'r' "
                        "GROUP BY i.relname, ix.indisunique, ix.indisprimary, am.amname",
                        tableName);

        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            Index idx;
            idx.name = row.get<std::string>(0);
            idx.isUnique = row.get<bool>(1);
            idx.isPrimary = row.get<bool>(2);
            idx.type = row.get<std::string>(3);

            // Split column names
            std::string colNames = row.get<std::string>(4);
            std::stringstream ss(colNames);
            std::string col;
            while (std::getline(ss, col, ',')) {
                idx.columns.push_back(col);
            }

            indexes.push_back(idx);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table indexes: " << e.what() << std::endl;
    }

    return indexes;
}

std::vector<ForeignKey> PostgresDatabase::getTableForeignKeys(const std::string& tableName) {
    std::vector<ForeignKey> foreignKeys;

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            std::format("SELECT "
                        "    tc.constraint_name, "
                        "    kcu.column_name as source_column, "
                        "    ccu.table_name as target_table, "
                        "    ccu.column_name as target_column, "
                        "    rc.update_rule, "
                        "    rc.delete_rule "
                        "FROM information_schema.table_constraints tc "
                        "JOIN information_schema.key_column_usage kcu "
                        "    ON tc.constraint_name = kcu.constraint_name "
                        "    AND tc.table_schema = kcu.table_schema "
                        "JOIN information_schema.constraint_column_usage ccu "
                        "    ON ccu.constraint_name = tc.constraint_name "
                        "    AND ccu.table_schema = tc.table_schema "
                        "JOIN information_schema.referential_constraints rc "
                        "    ON rc.constraint_name = tc.constraint_name "
                        "    AND rc.constraint_schema = tc.table_schema "
                        "WHERE tc.constraint_type = 'FOREIGN KEY' "
                        "AND tc.table_name = '{}' "
                        "AND tc.table_schema = 'public'",
                        tableName);

        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            ForeignKey fk;
            fk.name = row.get<std::string>(0);
            fk.sourceColumn = row.get<std::string>(1);
            fk.targetTable = row.get<std::string>(2);
            fk.targetColumn = row.get<std::string>(3);
            fk.onUpdate = row.get<std::string>(4);
            fk.onDelete = row.get<std::string>(5);

            foreignKeys.push_back(fk);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting table foreign keys: " << e.what() << std::endl;
    }

    return foreignKeys;
}

// View management methods
void PostgresDatabase::refreshViews() {
    LogPanel::debug("Refreshing views for database: " + name);
    if (!isConnected()) {
        LogPanel::warn("Not connected to database, cannot refresh views");
        getCurrentDatabaseData().viewsLoaded = true;
        return;
    }

    startRefreshViewAsync();
}

const std::vector<Table>& PostgresDatabase::getViews() const {
    return getCurrentDatabaseData().views;
}

std::vector<Table>& PostgresDatabase::getViews() {
    return getCurrentDatabaseData().views;
}

bool PostgresDatabase::areViewsLoaded() const {
    return getCurrentDatabaseData().viewsLoaded;
}

void PostgresDatabase::setViewsLoaded(const bool loaded) {
    getCurrentDatabaseData().viewsLoaded = loaded;
}

// Sequence management methods
void PostgresDatabase::refreshSequences() {
    LogPanel::debug("Refreshing sequences for database: " + name);
    if (!isConnected()) {
        LogPanel::warn("Not connected to database, cannot refresh sequences");
        getCurrentDatabaseData().sequencesLoaded = true;
        return;
    }

    startRefreshSequenceAsync();
}

const std::vector<std::string>& PostgresDatabase::getSequences() const {
    return getCurrentDatabaseData().sequences;
}

std::vector<std::string>& PostgresDatabase::getSequences() {
    return getCurrentDatabaseData().sequences;
}

bool PostgresDatabase::areSequencesLoaded() const {
    return getCurrentDatabaseData().sequencesLoaded;
}

void PostgresDatabase::setSequencesLoaded(const bool loaded) {
    getCurrentDatabaseData().sequencesLoaded = loaded;
}

std::vector<std::string> PostgresDatabase::getViewNames() {
    std::vector<std::string> viewNames;

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            "SELECT viewname FROM pg_views WHERE schemaname = 'public' ORDER BY viewname";

        std::cout << "Executing query to get view names..." << std::endl;
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            auto viewName = row.get<std::string>(0);
            std::cout << "Found view: " << viewName << std::endl;
            viewNames.push_back(viewName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << viewNames.size() << " views." << std::endl;
    return viewNames;
}

std::vector<Column> PostgresDatabase::getViewColumns(const std::string& viewName) {
    std::vector<Column> columns;

    try {
        const auto session = getSession();
        const std::string sqlQuery = "SELECT c.column_name, c.data_type, c.is_nullable "
                                     "FROM information_schema.columns c "
                                     "WHERE c.table_name = '" +
                                     viewName +
                                     "' "
                                     "ORDER BY c.ordinal_position";

        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            Column col;
            col.name = row.get<std::string>(0);
            col.type = row.get<std::string>(1);
            col.isNotNull = row.get<std::string>(2) == "NO";
            col.isPrimaryKey = false; // Views don't have primary keys
            columns.push_back(col);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting view columns: " << e.what() << std::endl;
    }

    return columns;
}

std::vector<std::string> PostgresDatabase::getSequenceNames() {
    std::vector<std::string> sequenceNames;

    try {
        const auto session = getSession();
        const std::string sqlQuery =
            "SELECT sequencename FROM pg_sequences WHERE schemaname = 'public' ORDER "
            "BY sequencename";

        std::cout << "Executing query to get sequence names..." << std::endl;
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            auto sequenceName = row.get<std::string>(0);
            std::cout << "Found sequence: " << sequenceName << std::endl;
            sequenceNames.push_back(sequenceName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << sequenceNames.size() << " sequences." << std::endl;
    return sequenceNames;
}

bool PostgresDatabase::isLoadingTables() const {
    return getCurrentDatabaseData().loadingTables;
}

void PostgresDatabase::checkTablesStatusAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (dbData.tablesFuture.valid() &&
        dbData.tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            dbData.tables = dbData.tablesFuture.get();
            LogPanel::info("Async table loading completed. Found " +
                           std::to_string(dbData.tables.size()) + " tables.");
            dbData.tablesLoaded = true;
            dbData.loadingTables = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async table loading: " << e.what() << std::endl;
            dbData.tablesLoaded = true;
            dbData.loadingTables = false;
        }
    }
}

void PostgresDatabase::startRefreshTableAsync() {
    auto& dbData = getCurrentDatabaseData();
    // Clear previous results
    dbData.tables.clear();
    dbData.tablesLoaded = false;
    dbData.loadingTables = true;

    // Start async loading with std::async
    dbData.tablesFuture =
        std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> PostgresDatabase::getTablesWithColumnsAsync() {
    std::vector<Table> result;
    auto& dbData = getCurrentDatabaseData();

    // Check if we're still supposed to be loading
    if (!dbData.loadingTables.load()) {
        return result;
    }

    try {
        if (!dbData.loadingTables.load()) {
            return result;
        }

        // Get table names using the connection pool
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery =
            "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename";

        {
            auto session = getSession();
            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!dbData.loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        LogPanel::debug("Found " + std::to_string(tableNames.size()) +
                        " tables, loading columns...");

        if (tableNames.empty() || !dbData.loadingTables.load()) {
            return result;
        }

        // Build a single query to get all columns for all tables at once
        std::string sqlQuery =
            "SELECT "
            "c.table_name, "
            "c.column_name, "
            "c.data_type, "
            "c.is_nullable, "
            "CASE WHEN tc.constraint_type = 'PRIMARY KEY' THEN 'true' ELSE 'false' END "
            "as is_primary_key "
            "FROM information_schema.columns c "
            "LEFT JOIN information_schema.key_column_usage kcu ON c.column_name = "
            "kcu.column_name AND c.table_name = kcu.table_name "
            "LEFT JOIN information_schema.table_constraints tc ON kcu.constraint_name = "
            "tc.constraint_name "
            "WHERE c.table_name IN (";

        // Add table names to the query
        for (size_t i = 0; i < tableNames.size(); ++i) {
            sqlQuery += "'" + tableNames[i] + "'";
            if (i < tableNames.size() - 1) {
                sqlQuery += ", ";
            }
        }
        sqlQuery += ") ORDER BY c.table_name, c.ordinal_position";

        // Execute the query using the connection pool
        std::unordered_map<std::string, std::vector<Column>> tableColumns;
        {
            auto session = getSession();
            const soci::rowset rs = session->prepare << sqlQuery;

            for (const auto& row : rs) {
                if (!dbData.loadingTables.load()) {
                    break; // Stop processing if we should no longer be loading
                }

                auto tableName = row.get<std::string>(0);
                Column col;
                col.name = row.get<std::string>(1);
                col.type = row.get<std::string>(2);
                col.isNotNull = row.get<std::string>(3) == "NO";
                col.isPrimaryKey = row.get<std::string>(4) == "true";

                tableColumns[tableName].push_back(col);
            }
        }

        // Build the result tables
        for (const auto& tableName : tableNames) {
            if (!dbData.loadingTables.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table table;
            table.name = tableName;
            table.fullName = name + "." + database + ".public." +
                             tableName;              // PostgreSQL: connection.database.schema.table
            table.columns = tableColumns[tableName]; // Will be empty if table has no columns

            // Load indexes and foreign keys
            table.indexes = getTableIndexes(tableName);
            table.foreignKeys = getTableForeignKeys(tableName);
            buildForeignKeyLookup(table);

            result.push_back(table);
            LogPanel::debug("Loaded table: " + tableName + " with " +
                            std::to_string(table.columns.size()) + " columns");
        }

        populateIncomingForeignKeys(result);

    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting tables with columns: " << e.what() << std::endl;
    }

    return result;
}

bool PostgresDatabase::isLoadingViews() const {
    return getCurrentDatabaseData().loadingViews;
}

void PostgresDatabase::checkViewsStatusAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (dbData.viewsFuture.valid() &&
        dbData.viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            dbData.views = dbData.viewsFuture.get();
            LogPanel::info("Async view loading completed. Found " +
                           std::to_string(dbData.views.size()) + " views.");
            dbData.viewsLoaded = true;
            dbData.loadingViews = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async view loading: " << e.what() << std::endl;
            dbData.viewsLoaded = true;
            dbData.loadingViews = false;
        }
    }
}

void PostgresDatabase::startRefreshViewAsync() {
    auto& dbData = getCurrentDatabaseData();
    // Clear previous results
    dbData.views.clear();
    dbData.viewsLoaded = false;
    dbData.loadingViews = true;

    // Start async loading with std::async
    dbData.viewsFuture =
        std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> PostgresDatabase::getViewsWithColumnsAsync() {
    std::vector<Table> result;
    auto& dbData = getCurrentDatabaseData();

    // Check if we're still supposed to be loading
    if (!dbData.loadingViews.load()) {
        return result;
    }

    try {
        if (!dbData.loadingViews.load()) {
            return result;
        }

        // Get view names using the connection pool
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery =
            "SELECT viewname FROM pg_views WHERE schemaname = 'public' ORDER BY viewname";

        {
            auto sql = getSession();
            const soci::rowset viewRs = sql->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!dbData.loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        LogPanel::debug("Found " + std::to_string(viewNames.size()) + " views, loading columns...");

        if (viewNames.empty() || !dbData.loadingViews.load()) {
            return result;
        }

        // Build a single query to get all columns for all views at once
        std::string sqlQuery = "SELECT "
                               "c.table_name, "
                               "c.column_name, "
                               "c.data_type, "
                               "c.is_nullable "
                               "FROM information_schema.columns c "
                               "WHERE c.table_name IN (";

        // Add view names to the query
        for (size_t i = 0; i < viewNames.size(); ++i) {
            sqlQuery += "'" + viewNames[i] + "'";
            if (i < viewNames.size() - 1) {
                sqlQuery += ", ";
            }
        }
        sqlQuery += ") ORDER BY c.table_name, c.ordinal_position";

        // Execute the query using the connection pool
        std::unordered_map<std::string, std::vector<Column>> viewColumns;
        {
            auto session = getSession();
            const soci::rowset rs = session->prepare << sqlQuery;

            for (const auto& row : rs) {
                if (!dbData.loadingViews.load()) {
                    break; // Stop processing if we should no longer be loading
                }

                auto viewName = row.get<std::string>(0);
                Column col;
                col.name = row.get<std::string>(1);
                col.type = row.get<std::string>(2);
                col.isNotNull = row.get<std::string>(3) == "NO";
                col.isPrimaryKey = false; // Views don't have primary keys

                viewColumns[viewName].push_back(col);
            }
        }

        // Build the result views
        for (const auto& viewName : viewNames) {
            if (!dbData.loadingViews.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table view;
            view.name = viewName;
            view.fullName = name + "." + database + ".public." +
                            viewName;             // PostgreSQL: connection.database.schema.view
            view.columns = viewColumns[viewName]; // Will be empty if view has no columns
            result.push_back(view);
            LogPanel::debug("Loaded view: " + viewName + " with " +
                            std::to_string(view.columns.size()) + " columns");
        }

    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting views with columns: " << e.what() << std::endl;
    }

    return result;
}

bool PostgresDatabase::isLoadingSequences() const {
    return getCurrentDatabaseData().loadingSequences;
}

void PostgresDatabase::checkSequencesStatusAsync() {
    auto& dbData = getCurrentDatabaseData();
    if (dbData.sequencesFuture.valid() &&
        dbData.sequencesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            dbData.sequences = dbData.sequencesFuture.get();
            std::cout << "Async sequence loading completed. Found " << dbData.sequences.size()
                      << " sequences." << std::endl;
            dbData.sequencesLoaded = true;
            dbData.loadingSequences = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async sequence loading: " << e.what() << std::endl;
            dbData.sequencesLoaded = true;
            dbData.loadingSequences = false;
        }
    }
}

void PostgresDatabase::startRefreshSequenceAsync() {
    auto& dbData = getCurrentDatabaseData();
    // Clear previous results
    dbData.sequences.clear();
    dbData.sequencesLoaded = false;
    dbData.loadingSequences = true;

    // Start async loading with std::async
    dbData.sequencesFuture =
        std::async(std::launch::async, [this]() { return getSequencesAsync(); });
}

std::vector<std::string> PostgresDatabase::getSequencesAsync() const {
    std::vector<std::string> result;
    const auto& dbData = getCurrentDatabaseData();

    // Check if we're still supposed to be loading
    if (!dbData.loadingSequences.load()) {
        return result;
    }

    try {
        if (!dbData.loadingSequences.load()) {
            return result;
        }

        const std::string sqlQuery =
            "SELECT sequencename FROM pg_sequences WHERE schemaname = 'public' "
            "ORDER BY sequencename";

        std::cout << "Executing query to get sequence names..." << std::endl;
        const auto session = getSession();
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            if (!dbData.loadingSequences.load()) {
                break;
            }

            auto sequenceName = row.get<std::string>(0);
            std::cout << "Found sequence: " << sequenceName << std::endl;
            result.push_back(sequenceName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << result.size() << " sequences." << std::endl;
    return result;
}

bool PostgresDatabase::isConnecting() const {
    return connecting;
}

void PostgresDatabase::startConnectionAsync() {
    if (connecting || connected) {
        return;
    }

    connecting = true;

    // Start async connection with std::async
    connectionFuture = std::async(std::launch::async, [this]() { return connect(); });
}

void PostgresDatabase::checkConnectionStatusAsync() {
    if (connectionFuture.valid() &&
        connectionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto [success, error] = connectionFuture.get();
            connecting = false;

            if (success) {
                LogPanel::info("Async connection completed successfully for: " + name);
                // Connection state is already set by the synchronous connect() method
                // Start loading databases if showAllDatabases is enabled and not already loading
                if (showAllDatabases && !databasesLoaded && !loadingDatabases.load()) {
                    LogPanel::debug("Starting async database loading after async connection...");
                    refreshDatabaseNames();
                }
                // Refresh tables for SQLite only (PostgreSQL will do it lazily)
                if (getType() == DatabaseType::SQLITE) {
                    refreshTables();
                }
            } else {
                LogPanel::error("Async connection failed for: " + name + " - " + error);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in async connection: " << e.what() << std::endl;
            connecting = false;
            attemptedConnection = true;
            lastConnectionError = e.what();
        }
    }
}

// Async table data loading methods
void PostgresDatabase::startTableDataLoadAsync(const std::string& tableName, int limit, int offset,
                                               const std::string& whereClause) {
    const bool started = tableDataLoader.start(tableName, [this, tableName, limit, offset,
                                                           whereClause](TableDataLoadState& state) {
        try {
            if (!state.loading.load()) {
                return;
            }

            std::string dataQuery = "SELECT * FROM \"" + tableName + "\"";
            if (!whereClause.empty()) {
                dataQuery += " WHERE " + whereClause;
            }
            dataQuery += " LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset);

            {
                const auto session = getSession();
                const soci::rowset dataRs = session->prepare << dataQuery;

                for (const auto& row : dataRs) {
                    if (!state.loading.load()) {
                        break;
                    }

                    std::vector<std::string> rowData;
                    rowData.reserve(row.size());
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        rowData.emplace_back(convertRowValue(row, i));
                    }
                    state.tableData.push_back(std::move(rowData));
                }
            }

            if (!state.loading.load()) {
                return;
            }

            const std::string columnQuery =
                std::format("SELECT column_name FROM information_schema.columns "
                            "WHERE table_name = '{}' ORDER BY ordinal_position",
                            tableName);

            const auto columnSession = getSession();
            const soci::rowset columnRs = columnSession->prepare << columnQuery;
            for (const auto& row : columnRs) {
                if (!state.loading.load()) {
                    break;
                }
                state.columnNames.push_back(row.get<std::string>(0));
            }

            if (!state.loading.load()) {
                return;
            }

            std::string countQuery = std::format(R"(SELECT COUNT(*) FROM "{}")", tableName);
            if (!whereClause.empty()) {
                countQuery =
                    std::format(R"(SELECT COUNT(*) FROM "{}" WHERE {})", tableName, whereClause);
            }
            const auto countSession = getSession();
            *countSession << countQuery, soci::into(state.rowCount);
        } catch (const std::exception& e) {
            std::cerr << "Error in async table data load: " << e.what() << std::endl;
            state.tableData.clear();
            state.columnNames.clear();
            state.rowCount = 0;
            state.lastError = e.what();
        }
    });

    if (!started) {
        return;
    }
}

bool PostgresDatabase::isLoadingTableData(const std::string& tableName) const {
    return tableDataLoader.isLoading(tableName);
}

bool PostgresDatabase::isLoadingTableData() const {
    return tableDataLoader.isAnyLoading();
}

void PostgresDatabase::checkTableDataStatusAsync(const std::string& tableName) {
    tableDataLoader.check(tableName);
}

void PostgresDatabase::checkTableDataStatusAsync() {
    tableDataLoader.checkAll();
}

bool PostgresDatabase::hasTableDataResult(const std::string& tableName) const {
    return tableDataLoader.hasResult(tableName);
}

bool PostgresDatabase::hasTableDataResult() const {
    return tableDataLoader.hasAnyResult();
}

std::vector<std::vector<std::string>>
PostgresDatabase::getTableDataResult(const std::string& tableName) {
    return tableDataLoader.getTableData(tableName);
}

std::vector<std::vector<std::string>> PostgresDatabase::getTableDataResult() {
    return tableDataLoader.getFirstAvailableTableData();
}

std::vector<std::string> PostgresDatabase::getColumnNamesResult(const std::string& tableName) {
    return tableDataLoader.getColumnNames(tableName);
}

std::vector<std::string> PostgresDatabase::getColumnNamesResult() {
    return tableDataLoader.getFirstAvailableColumnNames();
}

int PostgresDatabase::getRowCountResult(const std::string& tableName) {
    return tableDataLoader.getRowCount(tableName);
}

int PostgresDatabase::getRowCountResult() {
    return tableDataLoader.getFirstAvailableRowCount();
}

void PostgresDatabase::clearTableDataResult(const std::string& tableName) {
    tableDataLoader.clear(tableName);
}

void PostgresDatabase::clearTableDataResult() {
    tableDataLoader.clearAll();
}

// Schema management methods
void PostgresDatabase::refreshSchemas() {
    std::cout << "Refreshing schemas for database: " << name << std::endl;
    if (!isConnected()) {
        std::cout << "Not connected to database, cannot refresh schemas" << std::endl;
        getCurrentDatabaseData().schemasLoaded = true;
        return;
    }

    startRefreshSchemaAsync();
}

const std::vector<Schema>& PostgresDatabase::getSchemas() const {
    return getCurrentDatabaseData().schemas;
}

std::vector<Schema>& PostgresDatabase::getSchemas() {
    return getCurrentDatabaseData().schemas;
}

bool PostgresDatabase::areSchemasLoaded() const {
    return getCurrentDatabaseData().schemasLoaded;
}

void PostgresDatabase::setSchemasLoaded(const bool loaded) {
    getCurrentDatabaseData().schemasLoaded = loaded;
}

bool PostgresDatabase::isLoadingSchemas() const {
    return getCurrentDatabaseData().loadingSchemas;
}

void PostgresDatabase::checkSchemasStatusAsync() {
    checkSchemasStatusAsync(database);
}

void PostgresDatabase::checkSchemasStatusAsync(const std::string& dbName) {
    auto& dbData = getDatabaseData(dbName);

    // Check the per-database schema future first (for parallel loading)
    auto schemaFutureIt = databaseSchemaFutures.find(dbName);
    if (schemaFutureIt != databaseSchemaFutures.end() && schemaFutureIt->second.valid() &&
        schemaFutureIt->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            dbData.schemas = schemaFutureIt->second.get();
            LogPanel::info(std::format(
                "Async parallel schema loading completed for database {}. Found {} schemas", dbName,
                dbData.schemas.size()));
            dbData.schemasLoaded = true;
            dbData.loadingSchemas = false;
            databaseSchemaFutures.erase(schemaFutureIt); // Clean up completed future
        } catch (const std::exception& e) {
            std::cerr << "Error in async parallel schema loading for database " << dbName << ": "
                      << e.what() << std::endl;
            dbData.schemasLoaded = true;
            dbData.loadingSchemas = false;
            databaseSchemaFutures.erase(schemaFutureIt);
        }
        return;
    }

    // Fall back to the regular schema future (for current database)
    if (dbData.schemasFuture.valid() &&
        dbData.schemasFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            dbData.schemas = dbData.schemasFuture.get();
            LogPanel::info(
                std::format("Async schema loading completed for database {}. Found {} schemas",
                            dbName, dbData.schemas.size()));
            dbData.schemasLoaded = true;
            dbData.loadingSchemas = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async schema loading for database " << dbName << ": " << e.what()
                      << std::endl;
            dbData.schemasLoaded = true;
            dbData.loadingSchemas = false;
        }
    }
}

void PostgresDatabase::startRefreshSchemaAsync() {
    auto& dbData = getCurrentDatabaseData();
    // Clear previous results
    dbData.schemas.clear();
    dbData.schemasLoaded = false;
    dbData.loadingSchemas = true;

    // Start async loading with std::async
    dbData.schemasFuture = std::async(std::launch::async, [this]() { return getSchemasAsync(); });
}

void PostgresDatabase::startSchemasLoadAsync(const std::string& dbName) {
    auto& dbData = getDatabaseData(dbName);

    // Don't start if already loading or loaded
    if (dbData.loadingSchemas.load() || dbData.schemasLoaded) {
        return;
    }

    // Check if we already have a future running for this database
    if (databaseSchemaFutures.contains(dbName)) {
        return;
    }

    dbData.loadingSchemas = true;

    // Start async loading with a separate session for this database
    databaseSchemaFutures[dbName] = std::async(
        std::launch::async, [this, dbName]() { return getSchemasForDatabaseAsync(dbName); });

    LogPanel::debug("Started parallel schema loading for database: " + dbName);
}

std::vector<Schema> PostgresDatabase::getSchemasForDatabaseAsync(const std::string& dbName) const {
    std::vector<Schema> result;
    auto& dbData = getDatabaseData(dbName);

    // Check if we're still supposed to be loading
    if (!dbData.loadingSchemas.load()) {
        return result;
    }

    try {
        // Ensure we have a connection pool for the specific database
        const auto* pool = getConnectionPoolForDatabase(dbName);
        if (!pool) {
            // If no pool exists for this database, create one temporarily
            const std::string dbConnectionString = buildConnectionString(dbName);
            const_cast<PostgresDatabase*>(this)->initializeConnectionPool(dbName,
                                                                          dbConnectionString);
        }

        if (!dbData.loadingSchemas.load()) {
            return result;
        }

        // Get schema names using the connection pool
        std::vector<std::string> schemaNames;
        const std::string sqlQuery =
            "SELECT schema_name FROM information_schema.schemata "
            "WHERE schema_name NOT IN ('information_schema', 'pg_catalog', 'pg_toast') "
            "AND schema_name NOT LIKE 'pg_temp_%' "
            "AND schema_name NOT LIKE 'pg_toast_temp_%' "
            "ORDER BY schema_name";

        {
            const auto session = getSession(dbName);
            const soci::rowset rs = session->prepare << sqlQuery;
            for (const auto& row : rs) {
                if (!dbData.loadingSchemas.load()) {
                    return result;
                }
                schemaNames.push_back(row.get<std::string>(0));
            }
        }

        LogPanel::debug("Found " + std::to_string(schemaNames.size()) + " schemas in database " +
                        dbName);

        if (schemaNames.empty() || !dbData.loadingSchemas.load()) {
            return result;
        }

        for (const auto& schemaName : schemaNames) {
            if (!dbData.loadingSchemas.load()) {
                break;
            }

            Schema schema;
            schema.name = schemaName;

            result.push_back(schema);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting schemas for database " << dbName << ": " << e.what()
                  << std::endl;
    }

    return result;
}

std::vector<Schema> PostgresDatabase::getSchemasAsync() const {
    std::vector<Schema> result;
    const auto& dbData = getCurrentDatabaseData();

    // Check if we're still supposed to be loading
    if (!dbData.loadingSchemas.load()) {
        return result;
    }

    try {
        if (!dbData.loadingSchemas.load()) {
            return result;
        }

        // Get schema names using the connection pool
        std::vector<std::string> schemaNames;
        const std::string sqlQuery =
            "SELECT schema_name FROM information_schema.schemata "
            "WHERE schema_name NOT IN ('information_schema', 'pg_catalog', 'pg_toast') "
            "AND schema_name NOT LIKE 'pg_temp_%' "
            "AND schema_name NOT LIKE 'pg_toast_temp_%' "
            "ORDER BY schema_name";

        {
            const auto session = getSession();
            const soci::rowset rs = session->prepare << sqlQuery;
            for (const auto& row : rs) {
                if (!dbData.loadingSchemas.load()) {
                    return result;
                }
                schemaNames.push_back(row.get<std::string>(0));
            }
        }

        std::cout << "Found " << schemaNames.size() << " schemas, loading objects..." << std::endl;

        if (schemaNames.empty() || !dbData.loadingSchemas.load()) {
            return result;
        }

        for (const auto& schemaName : schemaNames) {
            if (!dbData.loadingSchemas.load()) {
                break;
            }

            Schema schema;
            schema.name = schemaName;

            result.push_back(schema);
            std::cout << "Loaded schema: " << schemaName << std::endl;
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting schemas: " << e.what() << std::endl;
    }

    return result;
}

std::vector<std::string> PostgresDatabase::getSchemaNames() const {
    std::vector<std::string> schemaNames;
    const auto& dbData = getCurrentDatabaseData();

    try {
        if (!dbData.loadingSchemas.load()) {
            return schemaNames;
        }

        const auto session = getSession();
        const std::string sqlQuery =
            "SELECT schema_name FROM information_schema.schemata "
            "WHERE schema_name NOT IN ('information_schema', 'pg_catalog', 'pg_toast') "
            "AND schema_name NOT LIKE 'pg_temp_%' "
            "AND schema_name NOT LIKE 'pg_toast_temp_%' "
            "ORDER BY schema_name";

        std::cout << "Executing query to get schema names..." << std::endl;
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            if (!dbData.loadingSchemas.load()) {
                break;
            }

            auto schemaName = row.get<std::string>(0);
            std::cout << "Found schema: " << schemaName << std::endl;
            schemaNames.push_back(schemaName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute schema query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << schemaNames.size() << " schemas." << std::endl;
    return schemaNames;
}

std::vector<std::string> PostgresDatabase::getDatabaseNames() {
    if (databasesLoaded) {
        return availableDatabases;
    }

    if (!loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }

    return availableDatabases;
}

void PostgresDatabase::refreshDatabaseNames() {
    if (loadingDatabases.load()) {
        return;
    }

    startRefreshDatabasesAsync();
}

bool PostgresDatabase::isLoadingDatabases() const {
    return loadingDatabases.load();
}

void PostgresDatabase::checkDatabasesStatusAsync() {
    if (databasesFuture.valid() &&
        databasesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            availableDatabases = databasesFuture.get();
            std::cout << "Async database loading completed. Found " << availableDatabases.size()
                      << " databases." << std::endl;
            databasesLoaded = true;
            loadingDatabases = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async database loading: " << e.what() << std::endl;
            databasesLoaded = true;
            loadingDatabases = false;
        }
    }
}

void PostgresDatabase::startRefreshDatabasesAsync() {
    // clear previous results
    availableDatabases.clear();
    databasesLoaded = false;
    loadingDatabases = true;

    // start async loading with std::async
    databasesFuture = std::async(std::launch::async, [this]() { return getDatabaseNamesAsync(); });
}

std::vector<std::string> PostgresDatabase::getDatabaseNamesAsync() const {
    std::vector<std::string> result;

    // Check if we're still supposed to be loading
    if (!loadingDatabases.load()) {
        return result;
    }

    try {
        if (!loadingDatabases.load()) {
            return result;
        }

        const std::string sqlQuery =
            "SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname";

        std::cout << "Executing async query to get database names..." << std::endl;
        const auto session = getSession();
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            if (!loadingDatabases.load()) {
                break; // Stop processing if we should no longer be loading
            }

            auto dbName = row.get<std::string>(0);
            std::cout << "Found database: " << dbName << std::endl;
            result.push_back(dbName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute async database query: " << e.what() << std::endl;
    }

    LogPanel::info(std::format("Async query completed. Found: {} databases", result.size()));
    return result;
}

std::pair<bool, std::string> PostgresDatabase::switchToDatabase(const std::string& targetDatabase) {
    if (database == targetDatabase && connected) {
        return {true, ""}; // Already connected to the target database
    }

    std::string targetConnectionString = buildConnectionString(targetDatabase);
    auto* pool = getConnectionPoolForDatabase(targetDatabase);

    if (pool) {
        // update database name and connection string only after successful connection
        database = targetDatabase;
        connectionString = targetConnectionString;
        connected = true;
        LogPanel::debug("Reusing existing connection pool to database: " + targetDatabase);
        return {true, ""};
    }

    // Create new connection pool to the target database
    try {
        initializeConnectionPool(targetDatabase, targetConnectionString);

        // Update database name and connection string only after successful connection
        database = targetDatabase;
        connectionString = targetConnectionString;
        connected = true;
        LogPanel::info("Created new connection pool to database: " + targetDatabase);
        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    }
}

void PostgresDatabase::switchToDatabaseAsync(const std::string& targetDatabase) {
    if (isSwitchingDatabase()) {
        return;
    }

    targetDatabaseName = targetDatabase;
    switchingDatabase = true;

    // Start async database switching
    databaseSwitchFuture = std::async(
        std::launch::async, [this, targetDatabase]() { return switchToDatabase(targetDatabase); });
}

bool PostgresDatabase::isSwitchingDatabase() const {
    return switchingDatabase.load();
}

void PostgresDatabase::checkDatabaseSwitchStatusAsync() {
    if (databaseSwitchFuture.valid() &&
        databaseSwitchFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto [success, error] = databaseSwitchFuture.get();
            switchingDatabase = false;

            if (success) {
                LogPanel::info("Async database switch completed successfully to: " +
                               targetDatabaseName);

                // Auto-start loading schemas for the switched database if not already loaded
                const auto& targetDbData = getDatabaseData(targetDatabaseName);
                if (!targetDbData.schemasLoaded && !targetDbData.loadingSchemas) {
                    LogPanel::debug("Auto-starting schema loading after database switch to: " +
                                    targetDatabaseName);
                    refreshSchemas();
                }
            } else {
                LogPanel::error("Async database switch failed to: " + targetDatabaseName + " - " +
                                error);
                lastConnectionError = error;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in async database switch: " << e.what() << std::endl;
            switchingDatabase = false;
            lastConnectionError = e.what();
        }
    }
}

bool PostgresDatabase::isDatabaseExpanded(const std::string& dbName) const {
    return expandedDatabases.contains(dbName);
}

void PostgresDatabase::setDatabaseExpanded(const std::string& dbName, const bool expanded_) {
    if (expanded_) {
        expandedDatabases.insert(dbName);
    } else {
        expandedDatabases.erase(dbName);
    }
}

soci::connection_pool*
PostgresDatabase::getConnectionPoolForDatabase(const std::string& dbName) const {
    // std::lock_guard lock(sessionMutex);
    const auto it = connectionPools.find(dbName);
    return (it != connectionPools.end()) ? it->second.get() : nullptr;
}

void PostgresDatabase::initializeConnectionPool(const std::string& dbName,
                                                const std::string& connStr) {
    std::lock_guard lock(sessionMutex);

    // Don't recreate if pool already exists
    if (connectionPools.contains(dbName)) {
        return;
    }

    constexpr size_t poolSize = 3;
    auto pool = std::make_unique<soci::connection_pool>(poolSize);

    // Initialize connections in parallel for faster startup
    std::vector<std::future<void>> connectionFutures;
    connectionFutures.reserve(poolSize);

    for (size_t i = 0; i != poolSize; ++i) {
        connectionFutures.emplace_back(std::async(std::launch::async, [&pool, i, connStr]() {
            soci::session& sql = pool->at(i);
            sql.open(soci::postgresql, connStr);
        }));
    }

    // Wait for all connections to complete
    for (auto& future : connectionFutures) {
        future.wait();
    }

    connectionPools[dbName] = std::move(pool);
}

std::string PostgresDatabase::buildConnectionString(const std::string& dbName) const {
    std::stringstream ss;
    ss << "host=" << host << " port=" << port;

    if (!dbName.empty()) {
        ss << " dbname=" << dbName;
    } else {
        ss << " dbname=" << "postgres";
    }

    if (!username.empty()) {
        ss << " user=" << username;
    }

    if (!password.empty()) {
        ss << " password=" << password;
    }

    return ss.str();
}

std::unique_ptr<soci::session> PostgresDatabase::getSession(const std::string& dbName) const {
    const std::string targetDb = dbName.empty() ? database : dbName;
    auto* pool = getConnectionPoolForDatabase(targetDb);
    if (!pool) {
        throw std::runtime_error("Connection pool not available for database: " + targetDb);
    }
    auto res = std::make_unique<soci::session>(*pool);
    if (!res->is_connected()) {
        res->reconnect();
    }
    return res;
}
