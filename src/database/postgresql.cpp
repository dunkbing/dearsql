#include "database/postgresql.hpp"
#include "database/db.hpp"
#include "utils/logger.hpp"
#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <ranges>
#include <sstream>
#include <unordered_map>
#include <vector>

PostgresDatabase::PostgresDatabase(const DatabaseConnectionInfo& connInfo)
    : connectionInfo(connInfo), database(connInfo.database) {
    this->name = connInfo.name;
    connectionString = buildConnectionString(database);
    if (database.empty()) {
        this->database = "postgres";
    }
}

PostgresDatabase::~PostgresDatabase() {
    // Stop all async operations before cleaning up
    loadingDatabases = false;
    switchingDatabase = false;

    tableDataLoader.cancelAllAndWait();

    // Stop all per-database async operations
    for (auto& [dbName, dbDataPtr] : databaseDataCache) {
        if (!dbDataPtr)
            continue;
        auto& dbData = *dbDataPtr;
        dbData.loadingSchemas = false;

        // Wait for all schema-level futures to complete
        for (auto& schema : dbData.schemas) {
            schema->loadingTables = false;
            schema->loadingViews = false;
            schema->loadingSequences = false;

            if (schema->tablesFuture.valid()) {
                schema->tablesFuture.wait();
            }
            if (schema->viewsFuture.valid()) {
                schema->viewsFuture.wait();
            }
            if (schema->sequencesFuture.valid()) {
                schema->sequencesFuture.wait();
            }
        }

        // Wait for database-level futures
        if (dbData.schemasFuture.valid()) {
            dbData.schemasFuture.wait();
        }
    }

    if (databasesFuture.valid()) {
        databasesFuture.wait();
    }
    if (databaseSwitchFuture.valid()) {
        databaseSwitchFuture.wait();
    }

    // Wait for all per-database schema futures to complete
    for (auto& [dbName, dbDataPtr] : databaseDataCache) {
        if (!dbDataPtr)
            continue;
        auto& dbData = *dbDataPtr;
        if (dbData.schemasFuture.valid()) {
            dbData.schemasFuture.wait();
        }
    }

    PostgresDatabase::disconnect();
}

void PostgresDatabase::setConnectionInfo(const DatabaseConnectionInfo& info) {
    connectionInfo = info;
    database = info.database;
    name = info.name;
    connectionString = buildConnectionString(database);
    if (database.empty()) {
        database = "postgres";
    }
}

// Helper methods for per-database data access
PostgresDatabaseNode* PostgresDatabase::getCurrentDatabaseData() {
    auto it = databaseDataCache.find(database);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<PostgresDatabaseNode>();
        newData->name = database;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[database] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

const PostgresDatabaseNode* PostgresDatabase::getCurrentDatabaseData() const {
    const auto it = databaseDataCache.find(database);
    return (it != databaseDataCache.end()) ? it->second.get() : nullptr;
}

PostgresDatabaseNode* PostgresDatabase::getDatabaseData(const std::string& dbName) {
    auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        // Create new DatabaseData with the name set
        auto newData = std::make_unique<PostgresDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

const PostgresDatabaseNode* PostgresDatabase::getDatabaseData(const std::string& dbName) const {
    const auto it = databaseDataCache.find(dbName);
    return (it != databaseDataCache.end()) ? it->second.get() : nullptr;
}

// Helper methods for per-schema data access
PostgresSchemaNode& PostgresDatabase::getSchemaData(const std::string& schemaName) {
    auto* dbData = getCurrentDatabaseData();
    auto& ptr = dbData->schemaDataCache[schemaName];
    if (!ptr) {
        ptr = std::make_unique<PostgresSchemaNode>();
    }
    return *ptr;
}

const PostgresSchemaNode& PostgresDatabase::getSchemaData(const std::string& schemaName) const {
    static const PostgresSchemaNode emptyData;
    const auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return emptyData;
    const auto it = dbData->schemaDataCache.find(schemaName);
    return (it != dbData->schemaDataCache.end() && it->second) ? *it->second : emptyData;
}

PostgresSchemaNode& PostgresDatabase::getSchemaData(const std::string& dbName,
                                                    const std::string& schemaName) {
    auto* dbData = getDatabaseData(dbName);
    auto& ptr = dbData->schemaDataCache[schemaName];
    if (!ptr) {
        ptr = std::make_unique<PostgresSchemaNode>();
    }
    return *ptr;
}

const PostgresSchemaNode& PostgresDatabase::getSchemaData(const std::string& dbName,
                                                          const std::string& schemaName) const {
    static const PostgresSchemaNode emptyData;
    const auto* dbData = getDatabaseData(dbName);
    if (!dbData)
        return emptyData;
    const auto it = dbData->schemaDataCache.find(schemaName);
    return (it != dbData->schemaDataCache.end() && it->second) ? *it->second : emptyData;
}

void PostgresDatabase::checkSchemaViewsStatusAsync(const std::string& schemaName) {
    auto& schemaData = getSchemaData(schemaName);
    if (schemaData.viewsFuture.valid() &&
        schemaData.viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            schemaData.views = schemaData.viewsFuture.get();
            Logger::info("Async view loading completed for schema " + schemaName + ". Found " +
                         std::to_string(schemaData.views.size()) + " views.");
            schemaData.viewsLoaded = true;
            schemaData.loadingViews = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async view loading for schema " << schemaName << ": " << e.what()
                      << std::endl;
            schemaData.viewsLoaded = true;
            schemaData.loadingViews = false;
        }
    }
}

void PostgresDatabase::checkSchemaSequencesStatusAsync(const std::string& schemaName) {
    auto& schemaData = getSchemaData(schemaName);
    if (schemaData.sequencesFuture.valid() &&
        schemaData.sequencesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            schemaData.sequences = schemaData.sequencesFuture.get();
            Logger::info("Async sequence loading completed for schema " + schemaName + ". Found " +
                         std::to_string(schemaData.sequences.size()) + " sequences.");
            schemaData.sequencesLoaded = true;
            schemaData.loadingSequences = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async sequence loading for schema " << schemaName << ": "
                      << e.what() << std::endl;
            schemaData.sequencesLoaded = true;
            schemaData.loadingSequences = false;
        }
    }
}

std::pair<bool, std::string> PostgresDatabase::connect() {
    const auto* pool = getConnectionPoolForDatabase(database);
    if (connected && pool) {
        return {true, ""};
    }

    setAttemptedConnection(true);

    try {
        initializeConnectionPool(database, connectionString);
        Logger::info("Successfully connected to PostgreSQL database: " + database);
        connected = true;
        setLastConnectionError("");

        // Start loading databases immediately if showAllDatabases is enabled
        if (connectionInfo.showAllDatabases && !databasesLoaded && !loadingDatabases.load()) {
            Logger::debug("Starting async database loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::cerr << "Connection to database failed: " << e.what() << std::endl;
        std::lock_guard lock(sessionMutex);
        // Clear connection pool from DatabaseData
        auto it = databaseDataCache.find(database);
        if (it != databaseDataCache.end() && it->second) {
            it->second->connectionPool.reset();
        }
        connected = false;
        setLastConnectionError(e.what());
        return {false, e.what()};
    }
}

void PostgresDatabase::disconnect() {
    std::lock_guard lock(sessionMutex);
    // Clear all connection pools
    for (auto& [dbName, dbDataPtr] : databaseDataCache) {
        if (dbDataPtr) {
            dbDataPtr->connectionPool.reset();
        }
    }
    connected = false;
}

const std::string& PostgresDatabase::getName() const {
    return name;
}

const std::string& PostgresDatabase::getConnectionString() const {
    return connectionString;
}

DatabaseType PostgresDatabase::getType() const {
    return DatabaseType::POSTGRESQL;
}

const std::string& PostgresDatabase::getDatabaseName() const {
    return database;
}

void PostgresDatabase::refreshTables() {
    refreshTables("public");
}

void PostgresDatabase::refreshTables(const std::string& schemaName) {
    std::cout << ("Refreshing tables for database: " + name + " schema: " + schemaName) << "\n";
    if (!isConnected()) {
        Logger::warn("Not connected to database, cannot refresh tables");
        getSchemaData(schemaName).tablesLoaded = true;
        return;
    }

    startRefreshTableAsync(schemaName);
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
        Logger::error("[soci] Postgres Error: " + std::string(e.what()));
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

void* PostgresDatabase::getConnection() const {
    return getConnectionPoolForDatabase(database);
}

std::vector<std::string> PostgresDatabase::getTableNames(const std::string& schemaName) {
    std::vector<std::string> tableNames;

    try {
        const auto session = getSession();
        const std::string sqlQuery = std::format(
            "SELECT tablename FROM pg_tables WHERE schemaname = '{}' ORDER BY tablename",
            schemaName);

        std::cout << ("Executing query to get table names from schema: " + schemaName);
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            auto tableName = row.get<std::string>(0);
            Logger::debug("Found table: " + tableName);
            tableNames.push_back(tableName);
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << ("Query completed. Found " + std::to_string(tableNames.size()) + " tables.");
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
            auto colNames = row.get<std::string>(4);
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
    return getTableForeignKeys(tableName, "public");
}

std::vector<ForeignKey> PostgresDatabase::getTableForeignKeys(const std::string& tableName,
                                                              const std::string& schemaName) {
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
                        "AND tc.table_schema = '{}'",
                        tableName, schemaName);

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
    refreshViews("public");
}

void PostgresDatabase::refreshViews(const std::string& schemaName) {
    Logger::debug("Refreshing views for database: " + name + " schema: " + schemaName);
    if (!isConnected()) {
        Logger::warn("Not connected to database, cannot refresh views");
        getSchemaData(schemaName).viewsLoaded = true;
        return;
    }

    startRefreshViewAsync(schemaName);
}

// Sequence management methods
void PostgresDatabase::refreshSequences() {
    refreshSequences("public");
}

void PostgresDatabase::refreshSequences(const std::string& schemaName) {
    Logger::debug("Refreshing sequences for database: " + name + " schema: " + schemaName);
    if (!isConnected()) {
        Logger::warn("Not connected to database, cannot refresh sequences");
        getSchemaData(schemaName).sequencesLoaded = true;
        return;
    }

    startRefreshSequenceAsync(schemaName);
}

std::vector<std::string> PostgresDatabase::getViewNames() {
    return getViewNames("public");
}

std::vector<std::string> PostgresDatabase::getViewNames(const std::string& schemaName) {
    std::vector<std::string> viewNames;

    try {
        const auto session = getSession();
        const std::string sqlQuery = std::format(
            "SELECT viewname FROM pg_views WHERE schemaname = '{}' ORDER BY viewname", schemaName);

        std::cout << "Executing query to get view names from schema: " << schemaName << std::endl;
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
    return getSequenceNames("public");
}

std::vector<std::string> PostgresDatabase::getSequenceNames(const std::string& schemaName) {
    std::vector<std::string> sequenceNames;

    try {
        const auto session = getSession();
        const std::string sqlQuery = std::format(
            "SELECT sequencename FROM pg_sequences WHERE schemaname = '{}' ORDER BY sequencename",
            schemaName);

        std::cout << "Executing query to get sequence names from schema: " << schemaName
                  << std::endl;
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

// void PostgresDatabase::checkTablesStatusAsync() {
//     // Check all schemas for table loading completion
//     auto* dbData = getCurrentDatabaseData();
//     if (!dbData)
//         return;
//     for (auto& schema : dbData->schemas) {
//         // checkSchemaTablesStatusAsync(schema->name);
//     }
// }

void PostgresDatabase::startRefreshTableAsync(const std::string& schemaName) {
    auto& schemaData = getSchemaData(schemaName);
    // Clear previous results
    schemaData.tables.clear();
    schemaData.tablesLoaded = false;
    schemaData.loadingTables = true;

    // Start async loading with std::async
    schemaData.tablesFuture = std::async(
        std::launch::async, [this, schemaName]() { return getTablesWithColumnsAsync(schemaName); });
}

std::vector<Table> PostgresDatabase::getTablesWithColumnsAsync(const std::string& schemaName) {
    std::vector<Table> result;
    auto& schemaData = getSchemaData(schemaName);

    // Check if we're still supposed to be loading
    if (!schemaData.loadingTables.load()) {
        return result;
    }

    try {
        if (!schemaData.loadingTables.load()) {
            return result;
        }

        // Get table names using the connection pool
        std::vector<std::string> tableNames;
        const std::string tableNamesQuery = std::format(
            "SELECT tablename FROM pg_tables WHERE schemaname = '{}' ORDER BY tablename",
            schemaName);
        std::cout << "getTablesWithColumnsAsync: " << tableNamesQuery << "\n";

        {
            auto session = getSession();
            const soci::rowset tableRs = session->prepare << tableNamesQuery;
            for (const auto& row : tableRs) {
                if (!schemaData.loadingTables.load()) {
                    return result;
                }
                tableNames.push_back(row.get<std::string>(0));
            }
        }

        std::cout << ("Found " + std::to_string(tableNames.size()) + " tables, loading columns...")
                  << "\n";

        if (tableNames.empty() || !schemaData.loadingTables.load()) {
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
                if (!schemaData.loadingTables.load()) {
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
            if (!schemaData.loadingTables.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table table;
            table.name = tableName;
            table.fullName = name + "." + database + "." + schemaName + "." +
                             tableName;              // PostgreSQL: connection.database.schema.table
            table.columns = tableColumns[tableName]; // Will be empty if table has no columns

            // Load indexes and foreign keys
            table.indexes = getTableIndexes(tableName);
            table.foreignKeys = getTableForeignKeys(tableName, schemaName);
            buildForeignKeyLookup(table);

            result.push_back(table);
            Logger::debug("Loaded table: " + tableName + " with " +
                          std::to_string(table.columns.size()) + " columns");
        }

        populateIncomingForeignKeys(result);

    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting tables with columns: " << e.what() << std::endl;
    }

    return result;
}

void PostgresDatabase::checkViewsStatusAsync() {
    // Check all schemas for view loading completion
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;
    for (auto& schema : dbData->schemas) {
        checkSchemaViewsStatusAsync(schema->name);
    }
}

void PostgresDatabase::startRefreshViewAsync(const std::string& schemaName) {
    auto& schemaData = getSchemaData(schemaName);
    // Clear previous results
    schemaData.views.clear();
    schemaData.viewsLoaded = false;
    schemaData.loadingViews = true;

    // Start async loading with std::async
    schemaData.viewsFuture = std::async(
        std::launch::async, [this, schemaName]() { return getViewsWithColumnsAsync(schemaName); });
}

std::vector<Table> PostgresDatabase::getViewsWithColumnsAsync(const std::string& schemaName) {
    std::vector<Table> result;
    auto& schemaData = getSchemaData(schemaName);

    // Check if we're still supposed to be loading
    if (!schemaData.loadingViews.load()) {
        return result;
    }

    try {
        if (!schemaData.loadingViews.load()) {
            return result;
        }

        // Get view names using the connection pool
        std::vector<std::string> viewNames;
        const std::string viewNamesQuery = std::format(
            "SELECT viewname FROM pg_views WHERE schemaname = '{}' ORDER BY viewname", schemaName);

        {
            auto sql = getSession();
            const soci::rowset viewRs = sql->prepare << viewNamesQuery;
            for (const auto& row : viewRs) {
                if (!schemaData.loadingViews.load()) {
                    return result;
                }
                viewNames.push_back(row.get<std::string>(0));
            }
        }

        Logger::debug("Found " + std::to_string(viewNames.size()) + " views, loading columns...");

        if (viewNames.empty() || !schemaData.loadingViews.load()) {
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
                if (!schemaData.loadingViews.load()) {
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
            if (!schemaData.loadingViews.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table view;
            view.name = viewName;
            view.fullName = name + "." + database + "." + schemaName + "." +
                            viewName;             // PostgreSQL: connection.database.schema.view
            view.columns = viewColumns[viewName]; // Will be empty if view has no columns
            result.push_back(view);
            Logger::debug("Loaded view: " + viewName + " with " +
                          std::to_string(view.columns.size()) + " columns");
        }

    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting views with columns: " << e.what() << std::endl;
    }

    return result;
}

void PostgresDatabase::checkSequencesStatusAsync() {
    // Check all schemas for sequence loading completion
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;
    for (auto& schema : dbData->schemas) {
        checkSchemaSequencesStatusAsync(schema->name);
    }
}

void PostgresDatabase::startRefreshSequenceAsync(const std::string& schemaName) {
    auto& schemaData = getSchemaData(schemaName);
    // Clear previous results
    schemaData.sequences.clear();
    schemaData.sequencesLoaded = false;
    schemaData.loadingSequences = true;

    // Start async loading with std::async
    schemaData.sequencesFuture = std::async(
        std::launch::async, [this, schemaName]() { return getSequencesAsync(schemaName); });
}

std::vector<std::string> PostgresDatabase::getSequencesAsync(const std::string& schemaName) const {
    std::vector<std::string> result;
    const auto& schemaData = getSchemaData(schemaName);

    // Check if we're still supposed to be loading
    if (!schemaData.loadingSequences.load()) {
        return result;
    }

    try {
        if (!schemaData.loadingSequences.load()) {
            return result;
        }

        const std::string sqlQuery = std::format(
            "SELECT sequencename FROM pg_sequences WHERE schemaname = '{}' ORDER BY sequencename",
            schemaName);

        std::cout << "Executing query to get sequence names from schema: " << schemaName
                  << std::endl;
        const auto session = getSession();
        const soci::rowset rs = session->prepare << sqlQuery;

        for (const auto& row : rs) {
            if (!schemaData.loadingSequences.load()) {
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

// Schema management methods
void PostgresDatabase::refreshSchemas() {
    std::cout << "Refreshing schemas for database: " << name << std::endl;
    if (!isConnected()) {
        std::cout << "Not connected to database, cannot refresh schemas" << std::endl;
        auto* dbData = getCurrentDatabaseData();
        if (dbData) {
            dbData->schemasLoaded = true;
        }
        return;
    }

    startRefreshSchemaAsync();
}

const std::vector<std::unique_ptr<PostgresSchemaNode>>& PostgresDatabase::getSchemas() const {
    const auto* dbData = getCurrentDatabaseData();
    static const std::vector<std::unique_ptr<PostgresSchemaNode>> emptySchemas;
    return dbData ? dbData->schemas : emptySchemas;
}

std::vector<std::unique_ptr<PostgresSchemaNode>>& PostgresDatabase::getSchemas() {
    auto* dbData = getCurrentDatabaseData();
    static std::vector<std::unique_ptr<PostgresSchemaNode>> emptySchemas;
    return dbData ? dbData->schemas : emptySchemas;
}

bool PostgresDatabase::areSchemasLoaded() const {
    const auto* dbData = getCurrentDatabaseData();
    return dbData ? dbData->schemasLoaded : false;
}

void PostgresDatabase::setSchemasLoaded(const bool loaded) {
    auto* dbData = getCurrentDatabaseData();
    if (dbData) {
        dbData->schemasLoaded = loaded;
    }
}

bool PostgresDatabase::isLoadingSchemas() const {
    const auto* dbData = getCurrentDatabaseData();
    return dbData ? dbData->loadingSchemas.load() : false;
}

// void PostgresDatabase::checkSchemasStatusAsync() {
//     checkSchemasStatusAsync(database);
// }

void PostgresDatabase::startRefreshSchemaAsync() {
    auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return;

    // Clear previous results
    dbData->schemas.clear();
    dbData->schemasLoaded = false;
    dbData->loadingSchemas = true;

    // Start async loading with std::async
    dbData->schemasFuture = std::async(std::launch::async, [this]() { return getSchemasAsync(); });
}

std::vector<std::unique_ptr<PostgresSchemaNode>> PostgresDatabase::getSchemasAsync() const {
    std::vector<std::unique_ptr<PostgresSchemaNode>> result;
    const auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return result;

    // Check if we're still supposed to be loading
    if (!dbData->loadingSchemas.load()) {
        return result;
    }

    try {
        if (!dbData->loadingSchemas.load()) {
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
                if (!dbData->loadingSchemas.load()) {
                    return result;
                }
                schemaNames.push_back(row.get<std::string>(0));
            }
        }

        std::cout << "Found " << schemaNames.size() << " schemas, loading objects..." << std::endl;

        if (schemaNames.empty() || !dbData->loadingSchemas.load()) {
            return result;
        }

        for (const auto& schemaName : schemaNames) {
            if (!dbData->loadingSchemas.load()) {
                break;
            }

            auto schema = std::make_unique<PostgresSchemaNode>();
            schema->name = schemaName;

            result.push_back(std::move(schema));
            std::cout << "Loaded schema: " << schemaName << std::endl;
        }
    } catch (const soci::soci_error& e) {
        std::cerr << "Error getting schemas: " << e.what() << std::endl;
    }

    return result;
}

std::vector<std::string> PostgresDatabase::getSchemaNames() const {
    std::vector<std::string> schemaNames;
    const auto* dbData = getCurrentDatabaseData();
    if (!dbData)
        return schemaNames;

    try {
        if (!dbData->loadingSchemas.load()) {
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
            if (!dbData->loadingSchemas.load()) {
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

std::vector<std::string> PostgresDatabase::getDatabases() {
    if (databasesLoaded) {
        const auto keys = databaseDataCache | std::ranges::views::keys;
        std::vector<std::string> databases;
        databases.reserve(databaseDataCache.size());
        for (auto& [k, _] : databaseDataCache) {
            databases.push_back(k);
        }
        return databases;
    }

    if (!loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }

    const auto keys = databaseDataCache | std::ranges::views::keys;
    std::vector<std::string> databases;
    databases.reserve(databaseDataCache.size());
    for (auto& [k, _] : databaseDataCache) {
        databases.push_back(k);
    }
    return databases;
}

const std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>>&
PostgresDatabase::getDatabaseDataMap() {
    // Auto-load databases if not loaded and not currently loading
    if (!databasesLoaded && !loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void PostgresDatabase::refreshDatabaseNames() {
    if (loadingDatabases.load()) {
        return;
    }

    // clear previous results
    availableDatabases.clear();
    databasesLoaded = false;
    loadingDatabases = true;

    // start async loading with std::async
    databasesFuture = std::async(std::launch::async, [this]() { return getDatabaseNamesAsync(); });
}

bool PostgresDatabase::isLoadingDatabases() const {
    return loadingDatabases.load();
}

void PostgresDatabase::checkDatabasesStatusAsync() {
    if (databasesFuture.valid() &&
        databasesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            const auto& databases = databasesFuture.get();
            std::cout << "Async database loading completed. Found " << databases.size()
                      << " databases." << std::endl;

            // Populate databaseDataCache with all available databases
            for (const auto& dbName : databases) {
                auto it = databaseDataCache.find(dbName);
                if (it == databaseDataCache.end()) {
                    // Create new DatabaseData with the name set
                    auto newData = std::make_unique<PostgresDatabaseNode>();
                    newData->name = dbName;
                    newData->parentDb = this;
                    databaseDataCache[dbName] = std::move(newData);
                }
            }

            databasesLoaded = true;
            loadingDatabases = false;
        } catch (const std::exception& e) {
            std::cerr << "Error in async database loading: " << e.what() << std::endl;
            databasesLoaded = true;
            loadingDatabases = false;
        }
    }
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

        std::vector<std::string> conditions = {sql::eq("datistemplate", "false")};
        if (!connectionInfo.showAllDatabases) {
            conditions.push_back(sql::eq("datname", "'" + database + "'"));
        }

        const std::string whereClause = sql::and_(conditions);
        const std::string sqlQuery =
            std::format("SELECT datname FROM pg_database WHERE {} ORDER BY datname", whereClause);

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

    Logger::info(std::format("Async query completed. Found: {} databases", result.size()));
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
        Logger::debug("Reusing existing connection pool to database: " + targetDatabase);
        return {true, ""};
    }

    // Create new connection pool to the target database
    try {
        initializeConnectionPool(targetDatabase, targetConnectionString);

        // Update database name and connection string only after successful connection
        database = targetDatabase;
        connectionString = targetConnectionString;
        connected = true;
        Logger::info("Created new connection pool to database: " + targetDatabase);
        return {true, ""};
    } catch (const soci::soci_error& e) {
        std::cerr << "Failed to connect to database " << targetDatabase << ": " << e.what()
                  << std::endl;
        connected = false;
        setLastConnectionError(e.what());
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
                Logger::info("Async database switch completed successfully to: " +
                             targetDatabaseName);

                // Auto-start loading schemas for the switched database if not already loaded
                const auto* targetDbData = getDatabaseData(targetDatabaseName);
                if (targetDbData && !targetDbData->schemasLoaded && !targetDbData->loadingSchemas) {
                    Logger::debug("Auto-starting schema loading after database switch to: " +
                                  targetDatabaseName);
                    refreshSchemas();
                }
            } else {
                Logger::error("Async database switch failed to: " + targetDatabaseName + " - " +
                              error);
                setLastConnectionError(error);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in async database switch: " << e.what() << std::endl;
            switchingDatabase = false;
            setLastConnectionError(e.what());
        }
    }
}

soci::connection_pool*
PostgresDatabase::getConnectionPoolForDatabase(const std::string& dbName) const {
    std::lock_guard lock(sessionMutex);

    // Use nested DatabaseData structure
    auto it = databaseDataCache.find(dbName);
    if (it != databaseDataCache.end() && it->second && it->second->connectionPool) {
        return it->second->connectionPool.get();
    }
    return nullptr;
}

void PostgresDatabase::initializeConnectionPool(const std::string& dbName,
                                                const std::string& connStr) {
    std::lock_guard lock(sessionMutex);

    // Get or create PostgresDatabaseNode for this database
    auto* dbData = getDatabaseData(dbName);
    if (!dbData)
        return;

    // Don't recreate if pool already exists
    if (dbData->connectionPool) {
        return;
    }

    constexpr size_t poolSize = 3;
    auto pool = std::make_unique<soci::connection_pool>(poolSize);

    // Initialize connections in parallel for faster startup
    std::vector<std::future<void>> connectionFutures;
    connectionFutures.reserve(poolSize);

    for (size_t i = 0; i != poolSize; ++i) {
        connectionFutures.emplace_back(std::async(std::launch::async, [&pool, i, connStr]() {
            soci::session& session = pool->at(i);
            session.open(soci::postgresql, connStr);
        }));
    }

    // Wait for all connections to complete
    for (auto& future : connectionFutures) {
        future.wait();
    }

    // Store in PostgresDatabaseNode
    dbData->connectionPool = std::move(pool);
}

std::string PostgresDatabase::buildConnectionString(const std::string& dbName) const {
    std::stringstream ss;
    ss << "host=" << connectionInfo.host << " port=" << connectionInfo.port;

    if (!dbName.empty()) {
        ss << " dbname=" << dbName;
    } else {
        ss << " dbname=" << "postgres";
    }

    if (!connectionInfo.username.empty()) {
        ss << " user=" << connectionInfo.username;
    }

    if (!connectionInfo.password.empty()) {
        ss << " password=" << connectionInfo.password;
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
