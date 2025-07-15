#include "database/postgresql.hpp"
#include <iostream>
#include <memory>
#include <sstream>
#include <typeinfo>

PostgreSQLDatabase::PostgreSQLDatabase(const std::string &name, const std::string &host, int port,
                                       const std::string &database, const std::string &username,
                                       const std::string &password)
    : name(name), host(host), port(port), database(database), username(username),
      password(password) {

    // Build connection string
    std::stringstream ss;
    ss << "host=" << host << " port=" << port << " dbname=" << database << " user=" << username
       << " password=" << password;
    connectionString = ss.str();
}

PostgreSQLDatabase::~PostgreSQLDatabase() {
    disconnect();
}

std::pair<bool, std::string> PostgreSQLDatabase::connect() {
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

void PostgreSQLDatabase::disconnect() {
    if (session) {
        session.reset();
    }
    connected = false;
}

bool PostgreSQLDatabase::isConnected() const {
    return connected && session;
}

const std::string &PostgreSQLDatabase::getName() const {
    return name;
}

const std::string &PostgreSQLDatabase::getConnectionString() const {
    return connectionString;
}

const std::string &PostgreSQLDatabase::getPath() const {
    return connectionString; // For PostgreSQL, path is the connection string
}

DatabaseType PostgreSQLDatabase::getType() const {
    return DatabaseType::POSTGRESQL;
}

void PostgreSQLDatabase::refreshTables() {
    std::cout << "Refreshing tables for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            tablesLoaded = true;
            return;
        }
    }

    tables.clear();
    const std::vector<std::string> tableNames = getTableNames();
    std::cout << "Found " << tableNames.size() << " tables" << std::endl;

    for (const auto &tableName : tableNames) {
        std::cout << "Adding table: " << tableName << std::endl;
        Table table;
        table.name = tableName;
        table.columns = getTableColumns(tableName);
        tables.push_back(table);
    }
    std::cout << "Finished refreshing tables. Total tables: " << tables.size() << std::endl;
    tablesLoaded = true;
}

const std::vector<Table> &PostgreSQLDatabase::getTables() const {
    return tables;
}

std::vector<Table> &PostgreSQLDatabase::getTables() {
    return tables;
}

bool PostgreSQLDatabase::areTablesLoaded() const {
    return tablesLoaded;
}

void PostgreSQLDatabase::setTablesLoaded(bool loaded) {
    tablesLoaded = loaded;
}

std::string PostgreSQLDatabase::executeQuery(const std::string &query) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            return "Error: Failed to connect to database: " + error;
        }
    }

    try {
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

std::vector<std::vector<std::string>> PostgreSQLDatabase::getTableData(const std::string &tableName,
                                                                       int limit, int offset) {
    std::vector<std::vector<std::string>> data;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return data;
        }
    }

    try {
        const std::string sql = "SELECT * FROM \"" + tableName + "\" LIMIT " +
                                std::to_string(limit) + " OFFSET " + std::to_string(offset);

        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            std::vector<std::string> rowData;
            for (std::size_t i = 0; i < row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.emplace_back("NULL");
                } else {
                    try {
                        rowData.push_back(row.get<std::string>(i));
                    } catch (const std::bad_cast &) {
                        rowData.emplace_back("[BINARY DATA]");
                    }
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting table data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> PostgreSQLDatabase::getColumnNames(const std::string &tableName) {
    std::vector<std::string> columnNames;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return columnNames;
        }
    }

    try {
        std::string sql =
            "SELECT column_name FROM information_schema.columns WHERE table_name = '" + tableName +
            "' ORDER BY ordinal_position";

        soci::rowset<soci::row> rs = session->prepare << sql;

        for (const auto &row : rs) {
            columnNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting column names: " << e.what() << std::endl;
    }

    return columnNames;
}

int PostgreSQLDatabase::getRowCount(const std::string &tableName) {
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cerr << "Failed to connect: " << error << std::endl;
            return 0;
        }
    }

    try {
        std::string sql = "SELECT COUNT(*) FROM \"" + tableName + "\"";
        int count = 0;
        *session << sql, soci::into(count);
        return count;
    } catch (const soci::soci_error &e) {
        std::cerr << "Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

bool PostgreSQLDatabase::isExpanded() const {
    return expanded;
}

void PostgreSQLDatabase::setExpanded(bool exp) {
    expanded = exp;
}

bool PostgreSQLDatabase::hasAttemptedConnection() const {
    return attemptedConnection;
}

void PostgreSQLDatabase::setAttemptedConnection(bool attempted) {
    attemptedConnection = attempted;
}

const std::string &PostgreSQLDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void PostgreSQLDatabase::setLastConnectionError(const std::string &error) {
    lastConnectionError = error;
}

void *PostgreSQLDatabase::getConnection() const {
    return session.get();
}

std::vector<std::string> PostgreSQLDatabase::getTableNames() {
    std::vector<std::string> tableNames;

    try {
        std::string sql =
            "SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY tablename";

        std::cout << "Executing query to get table names..." << std::endl;
        soci::rowset<soci::row> rs = session->prepare << sql;

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

std::vector<Column> PostgreSQLDatabase::getTableColumns(const std::string &tableName) {
    std::vector<Column> columns;

    try {
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

        soci::rowset<soci::row> rs = session->prepare << sql;

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
void PostgreSQLDatabase::refreshViews() {
    std::cout << "Refreshing views for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            viewsLoaded = true;
            return;
        }
    }

    views.clear();
    std::vector<std::string> viewNames = getViewNames();
    std::cout << "Found " << viewNames.size() << " views" << std::endl;

    for (const auto &viewName : viewNames) {
        std::cout << "Adding view: " << viewName << std::endl;
        Table view;
        view.name = viewName;
        view.columns = getViewColumns(viewName);
        views.push_back(view);
    }
    std::cout << "Finished refreshing views. Total views: " << views.size() << std::endl;
    viewsLoaded = true;
}

const std::vector<Table> &PostgreSQLDatabase::getViews() const {
    return views;
}

std::vector<Table> &PostgreSQLDatabase::getViews() {
    return views;
}

bool PostgreSQLDatabase::areViewsLoaded() const {
    return viewsLoaded;
}

void PostgreSQLDatabase::setViewsLoaded(bool loaded) {
    viewsLoaded = loaded;
}

// Sequence management methods
void PostgreSQLDatabase::refreshSequences() {
    std::cout << "Refreshing sequences for database: " << name << std::endl;
    if (!isConnected()) {
        auto [success, error] = connect();
        if (!success) {
            std::cout << "Failed to connect to database: " << error << std::endl;
            sequencesLoaded = true;
            return;
        }
    }

    sequences.clear();
    sequences = getSequenceNames();
    std::cout << "Found " << sequences.size() << " sequences" << std::endl;
    sequencesLoaded = true;
}

const std::vector<std::string> &PostgreSQLDatabase::getSequences() const {
    return sequences;
}

std::vector<std::string> &PostgreSQLDatabase::getSequences() {
    return sequences;
}

bool PostgreSQLDatabase::areSequencesLoaded() const {
    return sequencesLoaded;
}

void PostgreSQLDatabase::setSequencesLoaded(bool loaded) {
    sequencesLoaded = loaded;
}

std::vector<std::string> PostgreSQLDatabase::getViewNames() {
    std::vector<std::string> viewNames;

    try {
        std::string sql =
            "SELECT viewname FROM pg_views WHERE schemaname = 'public' ORDER BY viewname";

        std::cout << "Executing query to get view names..." << std::endl;
        soci::rowset<soci::row> rs = session->prepare << sql;

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

std::vector<Column> PostgreSQLDatabase::getViewColumns(const std::string &viewName) {
    std::vector<Column> columns;

    try {
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

std::vector<std::string> PostgreSQLDatabase::getSequenceNames() {
    std::vector<std::string> sequenceNames;

    try {
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
