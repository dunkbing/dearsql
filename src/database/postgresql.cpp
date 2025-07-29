#include "database/postgresql.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <typeinfo>
#include <unordered_map>

PostgresDatabase::PostgresDatabase(const std::string &name, const std::string &host, const int port,
                                   const std::string &database, const std::string &username,
                                   const std::string &password, bool showAllDatabases)
    : name(name), host(host), port(port), database(database), username(username),
      password(password), showAllDatabases(showAllDatabases) {

    std::stringstream ss;

    ss << "host=" << host << " port=" << port;

    if (!database.empty()) {
        ss << " dbname=" << database;
    }

    if (!username.empty()) {
        ss << " user=" << username;
    }

    if (!password.empty()) {
        ss << " password=" << password;
    }

    connectionString = ss.str();
}

PostgresDatabase::~PostgresDatabase() {
    // Stop all async operations before cleaning up
    loadingTables = false;
    loadingViews = false;
    loadingSequences = false;
    loadingSchemas = false;
    connecting = false;
    loadingTableData = false;

    // Wait for all futures to complete
    if (tablesFuture.valid()) {
        tablesFuture.wait();
    }
    if (viewsFuture.valid()) {
        viewsFuture.wait();
    }
    if (sequencesFuture.valid()) {
        sequencesFuture.wait();
    }
    if (schemasFuture.valid()) {
        schemasFuture.wait();
    }
    if (connectionFuture.valid()) {
        connectionFuture.wait();
    }
    if (tableDataFuture.valid()) {
        tableDataFuture.wait();
    }

    PostgresDatabase::disconnect();
}

std::pair<bool, std::string> PostgresDatabase::connect() {
    std::cout << "Connection string: " << connectionString << std::endl;
    if (connected && session) {
        return {true, ""};
    }

    attemptedConnection = true;

    try {
        session = std::make_unique<soci::session>(soci::postgresql, connectionString);
        std::cout << "Successfully connected to PostgreSQL database: " << database << std::endl;
        connected = true;
        lastConnectionError.clear();
        return {true, ""};
    } catch (const soci::soci_error &e) {
        std::cerr << "Connection to database failed: " << e.what() << std::endl;
        session.reset();
        connected = false;
        lastConnectionError = e.what();
        return {false, e.what()};
    }
}

void PostgresDatabase::disconnect() {
    if (session) {
        session.reset();
    }
    connected = false;
}

bool PostgresDatabase::isConnected() const {
    return connected && session;
}

const std::string &PostgresDatabase::getName() const {
    return name;
}

const std::string &PostgresDatabase::getConnectionString() const {
    return connectionString;
}

const std::string &PostgresDatabase::getPath() const {
    return connectionString;
}

DatabaseType PostgresDatabase::getType() const {
    return DatabaseType::POSTGRESQL;
}

const std::string &PostgresDatabase::getDatabaseName() const {
    return database;
}

void PostgresDatabase::refreshTables() {
    std::cout << "Refreshing tables for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            tablesLoaded = true;
            return;
        }
    }

    startRefreshTableAsync();
}

const std::vector<Table> &PostgresDatabase::getTables() const {
    return tables;
}

std::vector<Table> &PostgresDatabase::getTables() {
    return tables;
}

bool PostgresDatabase::areTablesLoaded() const {
    return tablesLoaded;
}

void PostgresDatabase::setTablesLoaded(bool loaded) {
    tablesLoaded = loaded;
}

std::string PostgresDatabase::executeQuery(const std::string &query) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            return "Error: Failed to connect to database: " + error;
        }
    }

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return "Error: Database session is not available";
        }

        std::stringstream output;
        const soci::rowset rs = session->prepare << query;

        // Get column names if available
        auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row &firstRow = *it;
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
        for (const auto &row : rs) {
            if (rowCount >= 1000)
                break;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    output << "NULL";
                } else {
                    try {
                        output << row.get<std::string>(i);
                    } catch (const std::bad_cast &) {
                        output << "[BINARY DATA]";
                    }
                }
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

        return output.str();
    } catch (const soci::soci_error &e) {
        return "Error: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
PostgresDatabase::executeQueryStructured(const std::string &query) {
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> data;

    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            return {columnNames, data};
        }
    }

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return {columnNames, data};
        }

        const soci::rowset rs = session->prepare << query;

        // Get column names if available
        auto it = rs.begin();
        if (it != rs.end()) {
            const soci::row &firstRow = *it;
            for (std::size_t i = 0; i < firstRow.size(); ++i) {
                columnNames.push_back(firstRow.get_properties(i).get_name());
            }
        }

        int rowCount = 0;
        for (const auto &row : rs) {
            if (rowCount >= 1000)
                break;

            std::vector<std::string> rowData;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.push_back("NULL");
                } else {
                    try {
                        rowData.push_back(row.get<std::string>(i));
                    } catch (const std::bad_cast &) {
                        rowData.push_back("[BINARY DATA]");
                    }
                }
            }
            data.push_back(rowData);
            rowCount++;
        }

        return {columnNames, data};
    } catch (const soci::soci_error &e) {
        // Return empty result on error
        return {columnNames, data};
    }
}

std::vector<std::vector<std::string>>
PostgresDatabase::getTableData(const std::string &tableName, const int limit, const int offset) {
    std::vector<std::vector<std::string>> data;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return data;
        }
    }

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return data;
        }

        const std::string sql = "SELECT * FROM \"" + tableName + "\" LIMIT " +
                                std::to_string(limit) + " OFFSET " + std::to_string(offset);

        soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            std::vector<std::string> rowData;

            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                    continue;
                }
                switch (soci::column_properties cp = row.get_properties(i); cp.get_db_type()) {
                case soci::db_string:
                    rowData.emplace_back(row.get<std::string>(i));
                    break;
                case soci::db_wstring:
                    // convert to UTF-8 string
                    {
                        auto ws = row.get<std::wstring>(i);
                        std::string utf8_str(ws.begin(), ws.end());
                        rowData.emplace_back(utf8_str);
                    }
                    break;
                case soci::db_int8:
                    rowData.emplace_back(std::to_string(row.get<int8_t>(i)));
                    break;
                case soci::db_int16:
                    rowData.emplace_back(std::to_string(row.get<int16_t>(i)));
                    break;
                case soci::db_int32:
                    rowData.emplace_back(std::to_string(row.get<int32_t>(i)));
                    break;
                case soci::db_int64:
                    rowData.emplace_back(std::to_string(row.get<int64_t>(i)));
                    break;
                case soci::db_double:
                    rowData.emplace_back(std::to_string(row.get<double>(i)));
                    break;
                case soci::db_date: {
                    auto date = row.get<std::tm>(i);
                    char buffer[32];
                    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &date);
                    rowData.emplace_back(buffer);
                } break;
                case soci::db_blob:
                    rowData.emplace_back("[BINARY DATA]");
                    break;
                default:
                    try {
                        rowData.emplace_back(row.get<std::string>(i));
                    } catch (const std::bad_cast &) {
                        rowData.emplace_back("[UNKNOWN DATA TYPE]");
                    }
                    break;
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> PostgresDatabase::getColumnNames(const std::string &tableName) {
    std::vector<std::string> columnNames;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return columnNames;
        }
    }

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return columnNames;
        }

        const std::string sql = std::format("SELECT column_name FROM information_schema.columns "
                                            "WHERE table_name = '{}' ORDER BY ordinal_position",
                                            tableName);

        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            columnNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }

    return columnNames;
}

int PostgresDatabase::getRowCount(const std::string &tableName) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return 0;
        }
    }

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return 0;
        }

        const std::string sql = std::format(R"(SELECT COUNT(*) FROM "{}")", tableName);
        int count = 0;
        *session << sql, soci::into(count);
        return count;
    } catch (const soci::soci_error &e) {
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

const std::string &PostgresDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void PostgresDatabase::setLastConnectionError(const std::string &error) {
    lastConnectionError = error;
}

void *PostgresDatabase::getConnection() const {
    return session.get();
}

std::vector<std::string> PostgresDatabase::getTableNames() {
    std::vector<std::string> tableNames;

    try {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (!session) {
            return tableNames;
        }

        const std::string sql =
            "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename";

        std::cout << "Executing query to get table names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            auto tableName = row.get<std::string>(0);
            std::cout << "Found table: " << tableName << std::endl;
            tableNames.push_back(tableName);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << tableNames.size() << " tables." << std::endl;
    return tableNames;
}

std::vector<Column> PostgresDatabase::getTableColumns(const std::string &tableName) {
    std::vector<Column> columns;

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return columns;
        }

        const std::string sql =
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

        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            Column col;
            col.name = row.get<std::string>(0);
            col.type = row.get<std::string>(1);
            col.isNotNull = row.get<std::string>(2) == "NO";
            auto isPkStr = row.get<std::string>(3);
            col.isPrimaryKey = (isPkStr == "true");
            columns.push_back(col);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting table columns: " << e.what() << std::endl;
    }

    return columns;
}

// View management methods
void PostgresDatabase::refreshViews() {
    std::cout << "Refreshing views for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            viewsLoaded = true;
            return;
        }
    }

    startRefreshViewAsync();
}

const std::vector<Table> &PostgresDatabase::getViews() const {
    return views;
}

std::vector<Table> &PostgresDatabase::getViews() {
    return views;
}

bool PostgresDatabase::areViewsLoaded() const {
    return viewsLoaded;
}

void PostgresDatabase::setViewsLoaded(const bool loaded) {
    viewsLoaded = loaded;
}

// Sequence management methods
void PostgresDatabase::refreshSequences() {
    std::cout << "Refreshing sequences for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            sequencesLoaded = true;
            return;
        }
    }

    startRefreshSequenceAsync();
}

const std::vector<std::string> &PostgresDatabase::getSequences() const {
    return sequences;
}

std::vector<std::string> &PostgresDatabase::getSequences() {
    return sequences;
}

bool PostgresDatabase::areSequencesLoaded() const {
    return sequencesLoaded;
}

void PostgresDatabase::setSequencesLoaded(const bool loaded) {
    sequencesLoaded = loaded;
}

std::vector<std::string> PostgresDatabase::getViewNames() {
    std::vector<std::string> viewNames;

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return viewNames;
        }

        const std::string sql =
            "SELECT viewname FROM pg_views WHERE schemaname = 'public' ORDER BY viewname";

        std::cout << "Executing query to get view names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            auto viewName = row.get<std::string>(0);
            std::cout << "Found view: " << viewName << std::endl;
            viewNames.push_back(viewName);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << viewNames.size() << " views." << std::endl;
    return viewNames;
}

std::vector<Column> PostgresDatabase::getViewColumns(const std::string &viewName) {
    std::vector<Column> columns;

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return columns;
        }

        const std::string sql = "SELECT c.column_name, c.data_type, c.is_nullable "
                                "FROM information_schema.columns c "
                                "WHERE c.table_name = '" +
                                viewName +
                                "' "
                                "ORDER BY c.ordinal_position";

        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            Column col;
            col.name = row.get<std::string>(0);
            col.type = row.get<std::string>(1);
            col.isNotNull = row.get<std::string>(2) == "NO";
            col.isPrimaryKey = false; // Views don't have primary keys
            columns.push_back(col);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting view columns: " << e.what() << std::endl;
    }

    return columns;
}

std::vector<std::string> PostgresDatabase::getSequenceNames() {
    std::vector<std::string> sequenceNames;

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return sequenceNames;
        }

        const std::string sql =
            "SELECT sequencename FROM pg_sequences WHERE schemaname = 'public' ORDER "
            "BY sequencename";

        std::cout << "Executing query to get sequence names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            auto sequenceName = row.get<std::string>(0);
            std::cout << "Found sequence: " << sequenceName << std::endl;
            sequenceNames.push_back(sequenceName);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << sequenceNames.size() << " sequences." << std::endl;
    return sequenceNames;
}

bool PostgresDatabase::isLoadingTables() const {
    return loadingTables;
}

void PostgresDatabase::checkTablesStatusAsync() {
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tables = tablesFuture.get();
            std::cout << "Async table loading completed. Found " << tables.size() << " tables."
                      << std::endl;
            tablesLoaded = true;
            loadingTables = false;
        } catch (const std::exception &e) {
            std::cerr << "Error in async table loading: " << e.what() << std::endl;
            tablesLoaded = true;
            loadingTables = false;
        }
    }
}

void PostgresDatabase::startRefreshTableAsync() {
    // Clear previous results
    tables.clear();
    tablesLoaded = false;
    loadingTables = true;

    // Start async loading with std::async
    tablesFuture = std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> PostgresDatabase::getTablesWithColumnsAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingTables.load()) {
        return result;
    }

    // Get all table names first
    const std::vector<std::string> tableNames = getTableNames();
    std::cout << "Found " << tableNames.size() << " tables, loading columns..." << std::endl;

    if (tableNames.empty() || !loadingTables.load()) {
        return result;
    }

    // Build a single query to get all columns for all tables at once
    std::string sql = "SELECT "
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
        sql += "'" + tableNames[i] + "'";
        if (i < tableNames.size() - 1) {
            sql += ", ";
        }
    }
    sql += ") ORDER BY c.table_name, c.ordinal_position";

    try {
        std::lock_guard lock(sessionMutex);
        if (!session || !loadingTables.load()) {
            return result;
        }

        // Execute the query
        const soci::rowset rs = session->prepare << sql;

        // Group columns by table name
        std::unordered_map<std::string, std::vector<Column>> tableColumns;

        for (const auto &row : rs) {
            if (!loadingTables.load()) {
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

        // Build the result tables
        for (const auto &tableName : tableNames) {
            if (!loadingTables.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table table;
            table.name = tableName;
            table.columns = tableColumns[tableName]; // Will be empty if table has no columns
            result.push_back(table);
            std::cout << "Loaded table: " << tableName << " with " << table.columns.size()
                      << " columns" << std::endl;
        }

    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting tables with columns: " << e.what() << std::endl;
    }

    return result;
}

bool PostgresDatabase::isLoadingViews() const {
    return loadingViews;
}

void PostgresDatabase::checkViewsStatusAsync() {
    if (viewsFuture.valid() &&
        viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            views = viewsFuture.get();
            std::cout << "Async view loading completed. Found " << views.size() << " views."
                      << std::endl;
            viewsLoaded = true;
            loadingViews = false;
        } catch (const std::exception &e) {
            std::cerr << "Error in async view loading: " << e.what() << std::endl;
            viewsLoaded = true;
            loadingViews = false;
        }
    }
}

void PostgresDatabase::startRefreshViewAsync() {
    // Clear previous results
    views.clear();
    viewsLoaded = false;
    loadingViews = true;

    // Start async loading with std::async
    viewsFuture = std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> PostgresDatabase::getViewsWithColumnsAsync() {
    std::vector<Table> result;

    // Check if we're still supposed to be loading
    if (!loadingViews.load()) {
        return result;
    }

    // Get all view names first
    const std::vector<std::string> viewNames = getViewNames();
    std::cout << "Found " << viewNames.size() << " views, loading columns..." << std::endl;

    if (viewNames.empty() || !loadingViews.load()) {
        return result;
    }

    // Build a single query to get all columns for all views at once
    std::string sql = "SELECT "
                      "c.table_name, "
                      "c.column_name, "
                      "c.data_type, "
                      "c.is_nullable "
                      "FROM information_schema.columns c "
                      "WHERE c.table_name IN (";

    // Add view names to the query
    for (size_t i = 0; i < viewNames.size(); ++i) {
        sql += "'" + viewNames[i] + "'";
        if (i < viewNames.size() - 1) {
            sql += ", ";
        }
    }
    sql += ") ORDER BY c.table_name, c.ordinal_position";

    try {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (!session || !loadingViews.load()) {
            return result;
        }

        // Execute the query
        const soci::rowset rs = session->prepare << sql;

        // Group columns by view name
        std::unordered_map<std::string, std::vector<Column>> viewColumns;

        for (const auto &row : rs) {
            if (!loadingViews.load()) {
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

        // Build the result views
        for (const auto &viewName : viewNames) {
            if (!loadingViews.load()) {
                break; // Stop processing if we should no longer be loading
            }

            Table view;
            view.name = viewName;
            view.columns = viewColumns[viewName]; // Will be empty if view has no columns
            result.push_back(view);
            std::cout << "Loaded view: " << viewName << " with " << view.columns.size()
                      << " columns" << std::endl;
        }

    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting views with columns: " << e.what() << std::endl;
    }

    return result;
}

bool PostgresDatabase::isLoadingSequences() const {
    return loadingSequences;
}

void PostgresDatabase::checkSequencesStatusAsync() {
    if (sequencesFuture.valid() &&
        sequencesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            sequences = sequencesFuture.get();
            std::cout << "Async sequence loading completed. Found " << sequences.size()
                      << " sequences." << std::endl;
            sequencesLoaded = true;
            loadingSequences = false;
        } catch (const std::exception &e) {
            std::cerr << "Error in async sequence loading: " << e.what() << std::endl;
            sequencesLoaded = true;
            loadingSequences = false;
        }
    }
}

void PostgresDatabase::startRefreshSequenceAsync() {
    // Clear previous results
    sequences.clear();
    sequencesLoaded = false;
    loadingSequences = true;

    // Start async loading with std::async
    sequencesFuture = std::async(std::launch::async, [this]() { return getSequencesAsync(); });
}

std::vector<std::string> PostgresDatabase::getSequencesAsync() const {
    std::vector<std::string> result;

    // Check if we're still supposed to be loading
    if (!loadingSequences.load()) {
        return result;
    }

    try {
        std::lock_guard<std::mutex> lock(sessionMutex);
        if (!session || !loadingSequences.load()) {
            return result;
        }

        const std::string sql = "SELECT sequencename FROM pg_sequences WHERE schemaname = 'public' "
                                "ORDER BY sequencename";

        std::cout << "Executing query to get sequence names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            if (!loadingSequences.load()) {
                break;
            }

            auto sequenceName = row.get<std::string>(0);
            std::cout << "Found sequence: " << sequenceName << std::endl;
            result.push_back(sequenceName);
        }
    } catch (const soci::soci_error &e) {
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
                std::cout << "Async connection completed successfully for: " << name << std::endl;
                // Connection state is already set by the synchronous connect() method
                // Refresh tables for SQLite only (PostgreSQL will do it lazily)
                if (getType() == DatabaseType::SQLITE) {
                    refreshTables();
                }
            } else {
                std::cout << "Async connection failed for: " << name << " - " << error << std::endl;
                // Connection state and error are already set by the synchronous connect() method
            }
        } catch (const std::exception &e) {
            std::cerr << "Error in async connection: " << e.what() << std::endl;
            connecting = false;
            attemptedConnection = true;
            lastConnectionError = e.what();
        }
    }
}

// Async table data loading methods
void PostgresDatabase::startTableDataLoadAsync(const std::string &tableName, int limit,
                                               int offset) {
    if (loadingTableData.load()) {
        return; // Already loading
    }

    loadingTableData = true;
    hasTableDataReady = false;
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;

    // Start async operation that loads everything
    tableDataFuture = std::async(std::launch::async, [this, tableName, limit, offset]() {
        try {
            tableDataResult = getTableData(tableName, limit, offset);
            columnNamesResult = getColumnNames(tableName);
            rowCountResult = getRowCount(tableName);
        } catch (const std::exception &e) {
            std::cerr << "Error in async table data load: " << e.what() << std::endl;
            // Clear results on error
            tableDataResult.clear();
            columnNamesResult.clear();
            rowCountResult = 0;
        }
    });
}

bool PostgresDatabase::isLoadingTableData() const {
    return loadingTableData.load();
}

void PostgresDatabase::checkTableDataStatusAsync() {
    if (!loadingTableData.load()) {
        return;
    }

    if (tableDataFuture.valid() &&
        tableDataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tableDataFuture.get(); // This will throw if there was an exception
            hasTableDataReady = true;
            loadingTableData = false;
        } catch (const std::exception &e) {
            std::cerr << "Error loading table data: " << e.what() << std::endl;
            loadingTableData = false;
            hasTableDataReady = false;
            // Clear results on error
            tableDataResult.clear();
            columnNamesResult.clear();
            rowCountResult = 0;
        }
    }
}

bool PostgresDatabase::hasTableDataResult() const {
    return hasTableDataReady.load();
}

std::vector<std::vector<std::string>> PostgresDatabase::getTableDataResult() {
    if (hasTableDataReady.load()) {
        return tableDataResult;
    }
    return {};
}

std::vector<std::string> PostgresDatabase::getColumnNamesResult() {
    if (hasTableDataReady.load()) {
        return columnNamesResult;
    }
    return {};
}

int PostgresDatabase::getRowCountResult() {
    if (hasTableDataReady.load()) {
        return rowCountResult;
    }
    return 0;
}

void PostgresDatabase::clearTableDataResult() {
    hasTableDataReady = false;
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;
}

// Schema management methods
void PostgresDatabase::refreshSchemas() {
    std::cout << "Refreshing schemas for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            schemasLoaded = true;
            return;
        }
    }

    startRefreshSchemaAsync();
}

const std::vector<Schema> &PostgresDatabase::getSchemas() const {
    return schemas;
}

std::vector<Schema> &PostgresDatabase::getSchemas() {
    return schemas;
}

bool PostgresDatabase::areSchemasLoaded() const {
    return schemasLoaded;
}

void PostgresDatabase::setSchemasLoaded(bool loaded) {
    schemasLoaded = loaded;
}

bool PostgresDatabase::isLoadingSchemas() const {
    return loadingSchemas;
}

void PostgresDatabase::checkSchemasStatusAsync() {
    if (schemasFuture.valid() &&
        schemasFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            schemas = schemasFuture.get();
            std::cout << "Async schema loading completed. Found " << schemas.size() << " schemas."
                      << std::endl;
            schemasLoaded = true;
            loadingSchemas = false;
        } catch (const std::exception &e) {
            std::cerr << "Error in async schema loading: " << e.what() << std::endl;
            schemasLoaded = true;
            loadingSchemas = false;
        }
    }
}

void PostgresDatabase::startRefreshSchemaAsync() {
    // Clear previous results
    schemas.clear();
    schemasLoaded = false;
    loadingSchemas = true;

    // Start async loading with std::async
    schemasFuture = std::async(std::launch::async, [this]() { return getSchemasAsync(); });
}

std::vector<Schema> PostgresDatabase::getSchemasAsync() const {
    std::vector<Schema> result;

    // Check if we're still supposed to be loading
    if (!loadingSchemas.load()) {
        return result;
    }

    // Get all schema names first
    const std::vector<std::string> schemaNames = getSchemaNames();
    std::cout << "Found " << schemaNames.size() << " schemas, loading objects..." << std::endl;

    if (schemaNames.empty() || !loadingSchemas.load()) {
        return result;
    }

    for (const auto &schemaName : schemaNames) {
        if (!loadingSchemas.load()) {
            break;
        }

        Schema schema;
        schema.name = schemaName;

        // For now, we'll only load the schema structure, not the actual objects
        // Objects will be loaded on demand when the schema is expanded

        result.push_back(schema);
        std::cout << "Loaded schema: " << schemaName << std::endl;
    }

    return result;
}

std::vector<std::string> PostgresDatabase::getSchemaNames() const {
    std::vector<std::string> schemaNames;

    try {
        std::lock_guard lock(sessionMutex);
        if (!session || !loadingSchemas.load()) {
            return schemaNames;
        }

        const std::string sql =
            "SELECT schema_name FROM information_schema.schemata "
            "WHERE schema_name NOT IN ('information_schema', 'pg_catalog', 'pg_toast') "
            "AND schema_name NOT LIKE 'pg_temp_%' "
            "AND schema_name NOT LIKE 'pg_toast_temp_%' "
            "ORDER BY schema_name";

        std::cout << "Executing query to get schema names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            if (!loadingSchemas.load()) {
                break;
            }

            auto schemaName = row.get<std::string>(0);
            std::cout << "Found schema: " << schemaName << std::endl;
            schemaNames.push_back(schemaName);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute schema query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << schemaNames.size() << " schemas." << std::endl;
    return schemaNames;
}

std::vector<std::string> PostgresDatabase::getDatabaseNames() {
    if (databasesLoaded) {
        return availableDatabases;
    }

    availableDatabases.clear();

    try {
        std::lock_guard lock(sessionMutex);
        if (!session) {
            return availableDatabases;
        }

        const std::string sql =
            "SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname";

        std::cout << "Executing query to get database names..." << std::endl;
        const soci::rowset rs = session->prepare << sql;

        for (const auto &row : rs) {
            auto dbName = row.get<std::string>(0);
            std::cout << "Found database: " << dbName << std::endl;
            availableDatabases.push_back(dbName);
        }

        databasesLoaded = true;
    } catch (const soci::soci_error &e) {
        std::cerr << "Failed to execute database query: " << e.what() << std::endl;
    }

    std::cout << "Query completed. Found " << availableDatabases.size() << " databases."
              << std::endl;
    return availableDatabases;
}

std::pair<bool, std::string> PostgresDatabase::switchToDatabase(const std::string &targetDatabase) {
    if (database == targetDatabase && connected) {
        return {true, ""}; // Already connected to the target database
    }

    // Disconnect from current database
    disconnect();

    // Clear all cached data since we're switching databases
    tables.clear();
    views.clear();
    sequences.clear();
    schemas.clear();
    tablesLoaded = false;
    viewsLoaded = false;
    sequencesLoaded = false;
    schemasLoaded = false;

    // Update database name and connection string
    database = targetDatabase;
    std::stringstream ss;
    ss << "host=" << host << " port=" << port << " dbname=" << database;

    // Only add user parameter if username is not empty
    if (!username.empty()) {
        ss << " user=" << username;
    }

    // Only add password parameter if password is not empty
    if (!password.empty()) {
        ss << " password=" << password;
    }

    connectionString = ss.str();

    // Connect to the new database
    auto result = connect();

    std::cout << "Switched to database: " << targetDatabase << " (success: " << result.first << ")"
              << std::endl;
    return result;
}

bool PostgresDatabase::isDatabaseExpanded(const std::string &dbName) const {
    return expandedDatabases.find(dbName) != expandedDatabases.end();
}

void PostgresDatabase::setDatabaseExpanded(const std::string &dbName, bool expanded) {
    if (expanded) {
        expandedDatabases.insert(dbName);
    } else {
        expandedDatabases.erase(dbName);
    }
}
