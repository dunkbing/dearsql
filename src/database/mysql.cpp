#include "database/mysql.hpp"
#include <iostream>
#include <sstream>

MySQLDatabase::MySQLDatabase(const std::string &name, const std::string &host, int port,
                             const std::string &database, const std::string &username,
                             const std::string &password)
    : name(name), host(host), port(port), database(database), username(username),
      password(password) {
    // SOCI MySQL connection string format: "db=database host=hostname user=username
    // password=password"
    connectionString = "db=" + database + " host=" + host + " port=" + std::to_string(port);

    if (!username.empty()) {
        connectionString += " user=" + username;
    }

    if (!password.empty()) {
        connectionString += " password=" + password;
    }
}

MySQLDatabase::~MySQLDatabase() {
    disconnect();
    if (tablesThread.joinable()) {
        tablesThread.join();
    }
    if (viewsThread.joinable()) {
        viewsThread.join();
    }
    if (connectionThread.joinable()) {
        connectionThread.join();
    }
}

std::pair<bool, std::string> MySQLDatabase::connect() {
    if (connected && session) {
        return {true, ""};
    }

    try {
        session = std::make_unique<soci::session>(soci::mysql, connectionString);
        connected = true;
        return {true, ""};
    } catch (const soci::soci_error &e) {
        connected = false;
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    } catch (const std::exception &e) {
        connected = false;
        std::string error = "MySQL connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MySQLDatabase::disconnect() {
    session.reset();
    connected = false;
    tablesLoaded = false;
    viewsLoaded = false;
    sequencesLoaded = false;
}

bool MySQLDatabase::isConnected() const {
    return connected && session;
}

bool MySQLDatabase::isConnecting() const {
    return connecting.load();
}

void MySQLDatabase::startConnectionAsync() {
    if (connecting.load()) {
        return;
    }

    connecting.store(true);
    connectionFuture = std::async(std::launch::async, [this]() {
        auto result = connect();
        connecting.store(false);
        return result;
    });
}

void MySQLDatabase::checkConnectionStatusAsync() {
    if (connectionFuture.valid() &&
        connectionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = connectionFuture.get();
        setAttemptedConnection(true);
        if (!result.first) {
            setLastConnectionError(result.second);
        }
    }
}

const std::string &MySQLDatabase::getName() const {
    return name;
}

const std::string &MySQLDatabase::getConnectionString() const {
    return connectionString;
}

const std::string &MySQLDatabase::getPath() const {
    static std::string empty;
    return empty;
}

void *MySQLDatabase::getConnection() const {
    return session.get();
}

DatabaseType MySQLDatabase::getType() const {
    return DatabaseType::MYSQL;
}

void MySQLDatabase::refreshTables() {
    if (loadingTables.load()) {
        return;
    }
    startAsyncTableRefresh();
}

void MySQLDatabase::startAsyncTableRefresh() {
    if (loadingTables.load()) {
        return;
    }

    loadingTables.store(true);
    tablesFuture = std::async(std::launch::async, [this]() { return getTablesWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getTablesWithColumnsAsync() {
    if (!connect().first) {
        return {};
    }

    std::vector<Table> result;
    std::vector<std::string> tableNames = getTableNames();

    for (const auto &tableName : tableNames) {
        Table table;
        table.name = tableName;
        table.columns = getTableColumns(tableName);
        result.push_back(table);
    }

    return result;
}

void MySQLDatabase::checkTablesStatusAsync() {
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        tables = tablesFuture.get();
        tablesLoaded = true;
        loadingTables.store(false);
    }
}

const std::vector<Table> &MySQLDatabase::getTables() const {
    return tables;
}

std::vector<Table> &MySQLDatabase::getTables() {
    return tables;
}

bool MySQLDatabase::areTablesLoaded() const {
    return tablesLoaded;
}

void MySQLDatabase::setTablesLoaded(bool loaded) {
    tablesLoaded = loaded;
}

bool MySQLDatabase::isLoadingTables() const {
    return loadingTables.load();
}

void MySQLDatabase::refreshViews() {
    if (loadingViews.load()) {
        return;
    }
    startAsyncViewRefresh();
}

void MySQLDatabase::startAsyncViewRefresh() {
    if (loadingViews.load()) {
        return;
    }

    loadingViews.store(true);
    viewsFuture = std::async(std::launch::async, [this]() { return getViewsWithColumnsAsync(); });
}

std::vector<Table> MySQLDatabase::getViewsWithColumnsAsync() {
    if (!connect().first) {
        return {};
    }

    std::vector<Table> result;
    std::vector<std::string> viewNames = getViewNames();

    for (const auto &viewName : viewNames) {
        Table view;
        view.name = viewName;
        view.columns = getViewColumns(viewName);
        result.push_back(view);
    }

    return result;
}

void MySQLDatabase::checkViewsStatusAsync() {
    if (viewsFuture.valid() &&
        viewsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        views = viewsFuture.get();
        viewsLoaded = true;
        loadingViews.store(false);
    }
}

const std::vector<Table> &MySQLDatabase::getViews() const {
    return views;
}

std::vector<Table> &MySQLDatabase::getViews() {
    return views;
}

bool MySQLDatabase::areViewsLoaded() const {
    return viewsLoaded;
}

void MySQLDatabase::setViewsLoaded(bool loaded) {
    viewsLoaded = loaded;
}

bool MySQLDatabase::isLoadingViews() const {
    return loadingViews.load();
}

void MySQLDatabase::refreshSequences() {
    sequences.clear();
    sequencesLoaded = true;
}

const std::vector<std::string> &MySQLDatabase::getSequences() const {
    return sequences;
}

std::vector<std::string> &MySQLDatabase::getSequences() {
    return sequences;
}

bool MySQLDatabase::areSequencesLoaded() const {
    return sequencesLoaded;
}

void MySQLDatabase::setSequencesLoaded(bool loaded) {
    sequencesLoaded = loaded;
}

bool MySQLDatabase::isLoadingSequences() const {
    return false;
}

void MySQLDatabase::checkSequencesStatusAsync() {
    // No-op for MySQL
}

std::string MySQLDatabase::executeQuery(const std::string &query) {
    if (!connect().first) {
        return "Error: Not connected to database";
    }

    try {
        soci::rowset<soci::row> rs = (session->prepare << query);

        std::ostringstream result;
        bool first_row = true;

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;

            if (first_row) {
                for (std::size_t i = 0; i != row.size(); ++i) {
                    if (i > 0)
                        result << "\t";
                    result << row.get_properties(i).get_name();
                }
                result << "\n";
                first_row = false;
            }

            for (std::size_t i = 0; i != row.size(); ++i) {
                if (i > 0)
                    result << "\t";

                if (row.get_indicator(i) == soci::i_null) {
                    result << "NULL";
                } else {
                    result << row.get<std::string>(i, "");
                }
            }
            result << "\n";
        }

        return result.str();
    } catch (const soci::soci_error &e) {
        return "MySQL Error: " + std::string(e.what());
    } catch (const std::exception &e) {
        return "Error: " + std::string(e.what());
    }
}

std::vector<std::vector<std::string>> MySQLDatabase::getTableData(const std::string &tableName,
                                                                  int limit, int offset) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::vector<std::string>> data;
    try {
        std::string query = "SELECT * FROM `" + tableName + "` LIMIT " + std::to_string(limit) +
                            " OFFSET " + std::to_string(offset);
        soci::rowset<soci::row> rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            std::vector<std::string> rowData;

            for (std::size_t i = 0; i != row.size(); ++i) {
                if (row.get_indicator(i) == soci::i_null) {
                    rowData.push_back("");
                } else {
                    rowData.push_back(row.get<std::string>(i, ""));
                }
            }
            data.push_back(rowData);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting table data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> MySQLDatabase::getColumnNames(const std::string &tableName) {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> columns;
    try {
        std::string query = "SHOW COLUMNS FROM `" + tableName + "`";
        soci::rowset<soci::row> rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            columns.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting column names: " << e.what() << std::endl;
    }

    return columns;
}

int MySQLDatabase::getRowCount(const std::string &tableName) {
    if (!connect().first) {
        return 0;
    }

    try {
        int count = 0;
        std::string query = "SELECT COUNT(*) FROM `" + tableName + "`";
        *session << query, soci::into(count);
        return count;
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting row count: " << e.what() << std::endl;
        return 0;
    }
}

void MySQLDatabase::startTableDataLoadAsync(const std::string &tableName, int limit, int offset) {
    if (loadingTableData.load()) {
        return;
    }

    loadingTableData.store(true);
    hasTableDataReady.store(false);

    tableDataFuture = std::async(std::launch::async, [this, tableName, limit, offset]() {
        tableDataResult = getTableData(tableName, limit, offset);
        columnNamesResult = getColumnNames(tableName);
        rowCountResult = getRowCount(tableName);
        hasTableDataReady.store(true);
        loadingTableData.store(false);
    });
}

bool MySQLDatabase::isLoadingTableData() const {
    return loadingTableData.load();
}

void MySQLDatabase::checkTableDataStatusAsync() {
    // Status is managed by atomic flags
}

bool MySQLDatabase::hasTableDataResult() const {
    return hasTableDataReady.load();
}

std::vector<std::vector<std::string>> MySQLDatabase::getTableDataResult() {
    return tableDataResult;
}

std::vector<std::string> MySQLDatabase::getColumnNamesResult() {
    return columnNamesResult;
}

int MySQLDatabase::getRowCountResult() {
    return rowCountResult;
}

void MySQLDatabase::clearTableDataResult() {
    hasTableDataReady.store(false);
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;
}

bool MySQLDatabase::isExpanded() const {
    return expanded;
}

void MySQLDatabase::setExpanded(bool expanded) {
    this->expanded = expanded;
}

bool MySQLDatabase::hasAttemptedConnection() const {
    return attemptedConnection;
}

void MySQLDatabase::setAttemptedConnection(bool attempted) {
    attemptedConnection = attempted;
}

const std::string &MySQLDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void MySQLDatabase::setLastConnectionError(const std::string &error) {
    lastConnectionError = error;
}

std::vector<std::string> MySQLDatabase::getTableNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> tableNames;
    try {
        std::string query = "SHOW TABLES";
        soci::rowset<soci::row> rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            tableNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting table names: " << e.what() << std::endl;
    }

    return tableNames;
}

std::vector<Column> MySQLDatabase::getTableColumns(const std::string &tableName) {
    if (!connect().first) {
        return {};
    }

    std::vector<Column> columns;
    try {
        std::string query = "DESCRIBE `" + tableName + "`";
        soci::rowset<soci::row> rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            Column col;
            col.name = row.get<std::string>(0);                  // Field
            col.type = row.get<std::string>(1);                  // Type
            col.isNotNull = row.get<std::string>(2) == "NO";     // Null
            col.isPrimaryKey = row.get<std::string>(3) == "PRI"; // Key
            columns.push_back(col);
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting table columns: " << e.what() << std::endl;
    }

    return columns;
}

std::vector<std::string> MySQLDatabase::getViewNames() {
    if (!connect().first) {
        return {};
    }

    std::vector<std::string> viewNames;
    try {
        std::string query = "SHOW FULL TABLES WHERE Table_type = 'VIEW'";
        soci::rowset<soci::row> rs = (session->prepare << query);

        for (auto it = rs.begin(); it != rs.end(); ++it) {
            const soci::row &row = *it;
            viewNames.push_back(row.get<std::string>(0));
        }
    } catch (const soci::soci_error &e) {
        std::cerr << "MySQL Error getting view names: " << e.what() << std::endl;
    }

    return viewNames;
}

std::vector<Column> MySQLDatabase::getViewColumns(const std::string &viewName) {
    return getTableColumns(viewName);
}

std::vector<std::string> MySQLDatabase::getSequenceNames() {
    return {};
}
