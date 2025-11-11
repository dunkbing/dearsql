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

    // Stop all per-database async operations
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (!dbDataPtr)
            continue;
        auto& dbData = *dbDataPtr;
        dbData.loadingSchemas = false;

        // Wait for all schema-level futures to complete
        for (const auto& schema : dbData.schemas) {
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

    // Wait for all per-database schema futures to complete
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
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

std::pair<bool, std::string> PostgresDatabase::connect(bool forceRefresh) {
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
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
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

void* PostgresDatabase::getConnection() const {
    return getConnectionPoolForDatabase(database);
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
        auto& schemaData = getSchemaData(schemaName);
        schemaData.viewsLoaded = true;
        return;
    }

    auto& schemaData = getSchemaData(schemaName);
    schemaData.startViewsLoadAsync();
}

// Sequence management methods
void PostgresDatabase::refreshSequences() {
    refreshSequences("public");
}

void PostgresDatabase::refreshSequences(const std::string& schemaName) {
    Logger::debug("Refreshing sequences for database: " + name + " schema: " + schemaName);
    if (!isConnected()) {
        Logger::warn("Not connected to database, cannot refresh sequences");
        auto& schemaData = getSchemaData(schemaName);
        schemaData.sequencesLoaded = true;
        return;
    }

    auto& schemaData = getSchemaData(schemaName);
    schemaData.startSequencesLoadAsync();
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

    auto* dbData = getCurrentDatabaseData();
    if (dbData) {
        dbData->startSchemasLoadAsync();
    }
}

const std::vector<std::unique_ptr<PostgresSchemaNode>>& PostgresDatabase::getSchemas() const {
    const auto* dbData = getCurrentDatabaseData();
    static constexpr std::vector<std::unique_ptr<PostgresSchemaNode>> emptySchemas;
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

std::vector<std::string> PostgresDatabase::getDatabases() {
    if (databasesLoaded) {
        std::vector<std::string> databases;
        databases.reserve(databaseDataCache.size());
        for (const auto& k : databaseDataCache | std::views::keys) {
            databases.push_back(k);
        }
        return databases;
    }

    if (!loadingDatabases.load() && isConnected()) {
        refreshDatabaseNames();
    }

    std::vector<std::string> databases;
    databases.reserve(databaseDataCache.size());
    for (const auto& k : databaseDataCache | std::views::keys) {
        databases.push_back(k);
    }
    return databases;
}

const std::unordered_map<std::string, std::unique_ptr<PostgresDatabaseNode>>&
PostgresDatabase::getDatabaseDataMap() {
    // autoload databases if not loaded and not currently loading
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

void PostgresDatabase::triggerChildDbRefresh() {
    Logger::debug(std::format("Triggering child db refresh for connection: {}", name));

    // loop through all schemas and trigger refresh for tables, views, and sequences
    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            Logger::debug(std::format("Refreshing db: {}", dbDataPtr->name));
            dbDataPtr->startSchemasLoadAsync(true, true);
        }
    }

    Logger::info(
        std::format("Triggered refresh for {} schemas in database {}", databaseDataCache.size(), name));
}
