#include "database/mysql/mysql_database_node.hpp"
#include "database/db.hpp"
#include "database/mysql.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <mysql/mysql.h>
#include <ranges>
#include <spdlog/spdlog.h>

namespace {

    bool shouldApplySslCA(const DatabaseConnectionInfo& info) {
        if (info.sslCACertPath.empty()) {
            return false;
        }

        switch (info.sslmode) {
        case SslMode::Require:
        case SslMode::VerifyCA:
        case SslMode::VerifyFull:
        case SslMode::VerifyIdentity:
            return true;
        default:
            return false;
        }
    }

    // escape a MySQL identifier: double any embedded backticks
    std::string quoteMysqlId(const std::string& id) {
        std::string out = "`";
        out.reserve(id.size() + 2);
        for (char c : id) {
            if (c == '`')
                out += '`';
            out += c;
        }
        out += '`';
        return out;
    }

    struct MysqlResDeleter {
        void operator()(MYSQL_RES* r) const {
            if (r)
                mysql_free_result(r);
        }
    };
    using MysqlResPtr = std::unique_ptr<MYSQL_RES, MysqlResDeleter>;

    std::function<MYSQL*()> makeMysqlFactory(const DatabaseConnectionInfo& info) {
        return [info]() -> MYSQL* {
            MYSQL* conn = mysql_init(nullptr);
            if (!conn) {
                throw std::runtime_error("mysql_init failed");
            }

            constexpr unsigned int connectTimeoutSeconds = 5;
            mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeoutSeconds);

            if (shouldApplySslCA(info)) {
                mysql_options(conn, MYSQL_OPT_SSL_CA, info.sslCACertPath.c_str());
            }

            switch (info.sslmode) {
            case SslMode::Disable: {
                my_bool enforce = 0;
                mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enforce);
                break;
            }
            case SslMode::Require: {
                my_bool enforce = 1;
                mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enforce);
                break;
            }
            case SslMode::VerifyCA: {
                my_bool enforce = 1;
                mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enforce);
                break;
            }
            case SslMode::VerifyFull:
            case SslMode::VerifyIdentity: {
                my_bool enforce = 1;
                my_bool verify = 1;
                mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enforce);
                mysql_options(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
                break;
            }
            default:
                break;
            }

            unsigned long flags = CLIENT_MULTI_STATEMENTS;
            if (!mysql_real_connect(conn, info.host.c_str(), info.username.c_str(),
                                    info.password.c_str(), info.database.c_str(), info.port,
                                    nullptr, flags)) {
                std::string err = mysql_error(conn);
                mysql_close(conn);
                throw std::runtime_error("MySQL connection failed: " + err);
            }
            mysql_set_character_set(conn, "utf8mb4");
            return conn;
        };
    }

    StatementResult extractMysqlResult(MYSQL* conn, int rowLimit) {
        StatementResult result;

        MYSQL_RES* rawRes = mysql_store_result(conn);
        if (rawRes) {
            MysqlResPtr res(rawRes);
            unsigned int nFields = mysql_num_fields(res.get());
            MYSQL_FIELD* fields = mysql_fetch_fields(res.get());

            std::vector<bool> isBoolCol(nFields, false);
            for (unsigned int i = 0; i < nFields; i++) {
                result.columnNames.emplace_back(fields[i].name);
                isBoolCol[i] = (fields[i].type == MYSQL_TYPE_TINY && fields[i].length == 1);
            }

            int rowCount = 0;
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr && rowCount < rowLimit) {
                unsigned long* lengths = mysql_fetch_lengths(res.get());
                std::vector<std::string> rowData;
                rowData.reserve(nFields);
                for (unsigned int i = 0; i < nFields; i++) {
                    if (row[i] == nullptr) {
                        rowData.emplace_back("NULL");
                    } else if (isBoolCol[i]) {
                        rowData.emplace_back(row[i][0] == '1' ? BOOL_TRUE_SENTINEL
                                                              : BOOL_FALSE_SENTINEL);
                    } else {
                        rowData.emplace_back(row[i], lengths[i]);
                    }
                }
                result.tableData.push_back(std::move(rowData));
                rowCount++;
            }

            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            my_ulonglong totalRows = mysql_num_rows(res.get());
            if (static_cast<int>(totalRows) >= rowLimit) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
        } else {
            if (mysql_field_count(conn) == 0) {
                my_ulonglong affected = mysql_affected_rows(conn);
                if (affected != (my_ulonglong)-1) {
                    result.message = std::format("{} row(s) affected", affected);
                } else {
                    result.message = "Query executed successfully";
                }
            } else {
                result.success = false;
                result.errorMessage = mysql_error(conn);
            }
        }

        return result;
    }

} // namespace

void MySQLDatabaseNode::ensureConnectionPool() {
    if (!connectionPool && parentDb) {
        auto nodeInfo = parentDb->getConnectionInfo();
        nodeInfo.database = name;
        initializeConnectionPool(nodeInfo);
    }
}

void MySQLDatabaseNode::checkTablesStatusAsync() {
    tablesLoader.check([this](const std::vector<Table>& result) {
        tables = result;
        populateIncomingForeignKeys(tables);
        spdlog::debug("Async table loading completed for database {}. Found {} tables", name,
                      tables.size());
        tablesLoaded = true;
    });
}

void MySQLDatabaseNode::startTablesLoadAsync(bool forceRefresh) {
    spdlog::debug("startTablesLoadAsync for db: {}{}", name,
                  (forceRefresh ? " (force refresh)" : ""));
    if (!parentDb) {
        return;
    }

    if (tablesLoader.isRunning()) {
        return;
    }

    if (forceRefresh) {
        tables.clear();
        tablesLoaded = false;
        lastTablesError.clear();
    }

    if (!forceRefresh && tablesLoaded) {
        return;
    }

    tables.clear();

    tablesLoader.start([this]() { return getTablesAsync(); });
}

std::vector<Table> MySQLDatabaseNode::getTablesAsync() {
    std::vector<Table> result;

    if (!tablesLoader.isRunning()) {
        return result;
    }

    try {
        if (!tablesLoader.isRunning()) {
            return result;
        }

        auto session = getSession();
        MYSQL* conn = session.get();

        // Get table names
        std::vector<std::string> tableNames;
        if (mysql_query(conn, "SHOW TABLES") == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    if (!tablesLoader.isRunning()) {
                        return result;
                    }
                    if (row[0])
                        tableNames.emplace_back(row[0]);
                }
            }
        }

        spdlog::debug("Found {} tables in database {}", tableNames.size(), name);

        if (tableNames.empty() || !tablesLoader.isRunning()) {
            return result;
        }

        // Load table details
        for (const auto& tableName : tableNames) {
            if (!tablesLoader.isRunning()) {
                break;
            }

            Table table;
            table.name = tableName;
            table.schema = name;
            table.fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;

            // Get table columns via DESCRIBE
            const std::string columnsQuery = std::format("DESCRIBE `{}`", tableName);
            if (mysql_query(conn, columnsQuery.c_str()) == 0) {
                MysqlResPtr res(mysql_store_result(conn));
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res.get())) != nullptr) {
                        if (!tablesLoader.isRunning()) {
                            break;
                        }
                        Column col;
                        col.name = row[0] ? row[0] : "";                           // Field
                        col.type = row[1] ? row[1] : "";                           // Type
                        col.isNotNull = row[2] && std::string(row[2]) == "NO";     // Null
                        col.isPrimaryKey = row[3] && std::string(row[3]) == "PRI"; // Key
                        // row[4] = Default, row[5] = Extra
                        col.isAutoIncrement = row[5] && std::string(row[5]).find(
                                                            "auto_increment") != std::string::npos;
                        table.columns.push_back(col);
                    }
                }
            }
            // Consume any remaining results
            while (mysql_next_result(conn) == 0) {
                MYSQL_RES* extra = mysql_store_result(conn);
                if (extra)
                    mysql_free_result(extra);
            }

            // Get foreign keys
            const std::string fkQuery = std::format(
                "SELECT COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME, "
                "CONSTRAINT_NAME "
                "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
                "WHERE TABLE_SCHEMA = '{}' AND TABLE_NAME = '{}' AND REFERENCED_TABLE_NAME IS NOT "
                "NULL",
                name, tableName);
            if (mysql_query(conn, fkQuery.c_str()) == 0) {
                MysqlResPtr res(mysql_store_result(conn));
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res.get())) != nullptr) {
                        if (!tablesLoader.isRunning()) {
                            break;
                        }
                        ForeignKey fk;
                        fk.sourceColumn = row[0] ? row[0] : "";
                        fk.targetTable = row[1] ? row[1] : "";
                        fk.targetColumn = row[2] ? row[2] : "";
                        fk.name = row[3] ? row[3] : "";
                        table.foreignKeys.push_back(fk);
                    }
                }
            }
            while (mysql_next_result(conn) == 0) {
                MYSQL_RES* extra = mysql_store_result(conn);
                if (extra)
                    mysql_free_result(extra);
            }

            result.push_back(table);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting tables for database {}: {}", name, e.what());
        lastTablesError = e.what();
    }

    return result;
}

void MySQLDatabaseNode::checkViewsStatusAsync() {
    viewsLoader.check([this](const std::vector<Table>& result) {
        views = result;
        spdlog::debug("Async view loading completed for database {}. Found {} views", name,
                      views.size());
        viewsLoaded = true;
    });
}

void MySQLDatabaseNode::startViewsLoadAsync(bool forceRefresh) {
    spdlog::debug("startViewsLoadAsync for database: {}", name);
    if (!parentDb) {
        return;
    }

    if (viewsLoader.isRunning() || (viewsLoaded && !forceRefresh)) {
        return;
    }

    if (forceRefresh) {
        views.clear();
        viewsLoaded = false;
        lastViewsError.clear();
    }

    viewsLoader.start([this]() { return getViewsForDatabaseAsync(); });
}

std::vector<Table> MySQLDatabaseNode::getViewsForDatabaseAsync() {
    std::vector<Table> result;

    if (!viewsLoader.isRunning()) {
        return result;
    }

    try {
        if (!connectionPool) {
            auto nodeInfo = parentDb->getConnectionInfo();
            nodeInfo.database = name;
            initializeConnectionPool(nodeInfo);
        }

        if (!viewsLoader.isRunning()) {
            return result;
        }

        auto session = getSession();
        MYSQL* conn = session.get();

        // Get view names
        std::vector<std::string> viewNames;
        if (mysql_query(conn, "SHOW FULL TABLES WHERE Table_type = 'VIEW'") == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    if (!viewsLoader.isRunning()) {
                        return result;
                    }
                    if (row[0])
                        viewNames.emplace_back(row[0]);
                }
            }
        }

        spdlog::debug("Found {} views in database {}", viewNames.size(), name);

        if (viewNames.empty() || !viewsLoader.isRunning()) {
            return result;
        }

        // Load view details
        for (const auto& viewName : viewNames) {
            if (!viewsLoader.isRunning()) {
                break;
            }

            Table view;
            view.name = viewName;
            view.schema = name;
            view.fullName = parentDb->getConnectionInfo().name + "." + name + "." + viewName;

            const std::string columnsQuery = std::format("DESCRIBE `{}`", viewName);
            if (mysql_query(conn, columnsQuery.c_str()) == 0) {
                MysqlResPtr res(mysql_store_result(conn));
                if (res) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(res.get())) != nullptr) {
                        if (!viewsLoader.isRunning()) {
                            break;
                        }
                        Column col;
                        col.name = row[0] ? row[0] : "";
                        col.type = row[1] ? row[1] : "";
                        col.isNotNull = row[2] && std::string(row[2]) == "NO";
                        view.columns.push_back(col);
                    }
                }
            }
            while (mysql_next_result(conn) == 0) {
                MYSQL_RES* extra = mysql_store_result(conn);
                if (extra)
                    mysql_free_result(extra);
            }

            result.push_back(view);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting views for database {}: {}", name, e.what());
        lastViewsError = e.what();
    }

    return result;
}

void MySQLDatabaseNode::checkRoutinesStatusAsync() {
    routinesLoader.check([this](const std::vector<Routine>& result) {
        routines = result;
        spdlog::debug("Async routine loading completed for database {}. Found {} routines", name,
                      routines.size());
        routinesLoaded = true;
    });
}

void MySQLDatabaseNode::startRoutinesLoadAsync(bool forceRefresh) {
    spdlog::debug("startRoutinesLoadAsync for database: {}", name);
    if (!parentDb) {
        return;
    }

    if (routinesLoader.isRunning() || (routinesLoaded && !forceRefresh)) {
        return;
    }

    if (forceRefresh) {
        routines.clear();
        routinesLoaded = false;
        lastRoutinesError.clear();
    }

    routinesLoader.start([this]() { return getRoutinesAsync(); });
}

std::vector<Routine> MySQLDatabaseNode::getRoutinesAsync() {
    std::vector<Routine> result;

    if (!routinesLoader.isRunning()) {
        return result;
    }

    try {
        if (!connectionPool) {
            auto nodeInfo = parentDb->getConnectionInfo();
            nodeInfo.database = name;
            initializeConnectionPool(nodeInfo);
        }

        if (!routinesLoader.isRunning()) {
            return result;
        }

        auto session = getSession();
        MYSQL* conn = session.get();

        const std::string query = std::format(
            "SELECT ROUTINE_NAME, "
            "CONCAT(ROUTINE_NAME, '(', IFNULL("
            "(SELECT GROUP_CONCAT(PARAMETER_NAME, ' ', DATA_TYPE ORDER BY ORDINAL_POSITION) "
            "FROM information_schema.PARAMETERS p "
            "WHERE p.SPECIFIC_SCHEMA = r.ROUTINE_SCHEMA "
            "AND p.SPECIFIC_NAME = r.ROUTINE_NAME "
            "AND p.PARAMETER_MODE IS NOT NULL), ''), ')') AS signature, "
            "ROUTINE_TYPE, "
            "DTD_IDENTIFIER "
            "FROM information_schema.ROUTINES r "
            "WHERE ROUTINE_SCHEMA = '{}' "
            "ORDER BY ROUTINE_TYPE, ROUTINE_NAME",
            name);

        if (mysql_real_query(conn, query.c_str(), query.size()) == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    if (!routinesLoader.isRunning()) {
                        return result;
                    }
                    Routine routine;
                    routine.name = row[0] ? row[0] : "";
                    routine.signature = row[1] ? row[1] : "";
                    std::string routineType = row[2] ? row[2] : "";
                    routine.kind = (routineType == "PROCEDURE") ? RoutineKind::Procedure
                                                                : RoutineKind::Function;
                    routine.returnType = row[3] ? row[3] : "";
                    result.push_back(routine);
                }
            }
        } else {
            spdlog::error("Error querying routines for database {}: {}", name, mysql_error(conn));
            lastRoutinesError = mysql_error(conn);
        }
        // consume any remaining results
        while (mysql_next_result(conn) == 0) {
            MYSQL_RES* extra = mysql_store_result(conn);
            if (extra)
                mysql_free_result(extra);
        }

        spdlog::debug("Found {} routines in database {}", result.size(), name);
    } catch (const std::exception& e) {
        spdlog::error("Error getting routines for database {}: {}", name, e.what());
        lastRoutinesError = e.what();
    }

    return result;
}

void MySQLDatabaseNode::startTableRefreshAsync(const std::string& tableName) {
    spdlog::debug("Starting async refresh for table: {}", tableName);

    if (tableRefreshLoaders.contains(tableName) && tableRefreshLoaders[tableName].isRunning()) {
        return;
    }

    tableRefreshLoaders[tableName].start(
        [this, tableName]() { return refreshTableAsync(tableName); });
}

void MySQLDatabaseNode::checkTableRefreshStatusAsync(const std::string& tableName) {
    auto it = tableRefreshLoaders.find(tableName);
    if (it == tableRefreshLoaders.end()) {
        return;
    }

    it->second.check([this, tableName](const Table& refreshedTable) {
        const auto tableIt = std::ranges::find_if(
            tables, [&tableName](const Table& t) { return t.name == tableName; });

        if (tableIt != tables.end()) {
            *tableIt = refreshedTable;
            spdlog::debug("Table {} refreshed successfully", tableName);
        }

        tableRefreshLoaders.erase(tableName);
    });
}

Table MySQLDatabaseNode::refreshTableAsync(const std::string& tableName) {
    spdlog::debug("Refreshing table: {}", tableName);

    Table refreshedTable;
    refreshedTable.name = tableName;
    refreshedTable.schema = name;
    refreshedTable.fullName = parentDb->getConnectionInfo().name + "." + name + "." + tableName;

    try {
        auto session = getSession();
        MYSQL* conn = session.get();

        // Reload columns
        const std::string columnsQuery = std::format("DESCRIBE `{}`", tableName);
        if (mysql_query(conn, columnsQuery.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    Column col;
                    col.name = row[0] ? row[0] : "";
                    col.type = row[1] ? row[1] : "";
                    col.isNotNull = row[2] && std::string(row[2]) == "NO";
                    col.isPrimaryKey = row[3] && std::string(row[3]) == "PRI";
                    col.isAutoIncrement =
                        row[5] && std::string(row[5]).find("auto_increment") != std::string::npos;
                    refreshedTable.columns.push_back(col);
                }
            }
        }
        while (mysql_next_result(conn) == 0) {
            MYSQL_RES* extra = mysql_store_result(conn);
            if (extra)
                mysql_free_result(extra);
        }

        // Reload indexes
        const std::string indexQuery =
            std::format("SHOW INDEX FROM `{}` WHERE Key_name != 'PRIMARY'", tableName);
        if (mysql_query(conn, indexQuery.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                std::map<std::string, Index> indexMap;
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    std::string indexName = row[2] ? row[2] : "";
                    std::string columnName = row[4] ? row[4] : "";
                    int nonUnique = row[1] ? std::atoi(row[1]) : 1;

                    if (!indexMap.contains(indexName)) {
                        Index idx;
                        idx.name = indexName;
                        idx.isUnique = (nonUnique == 0);
                        indexMap[indexName] = idx;
                    }
                    indexMap[indexName].columns.push_back(columnName);
                }
                for (auto& idx : indexMap | std::views::values) {
                    refreshedTable.indexes.push_back(idx);
                }
            }
        }
        while (mysql_next_result(conn) == 0) {
            MYSQL_RES* extra = mysql_store_result(conn);
            if (extra)
                mysql_free_result(extra);
        }

        // Reload foreign keys
        const std::string fkQuery = std::format(
            "SELECT COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME, CONSTRAINT_NAME "
            "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
            "WHERE TABLE_SCHEMA = '{}' AND TABLE_NAME = '{}' AND REFERENCED_TABLE_NAME IS NOT NULL",
            name, tableName);
        if (mysql_query(conn, fkQuery.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    ForeignKey fk;
                    fk.sourceColumn = row[0] ? row[0] : "";
                    fk.targetTable = row[1] ? row[1] : "";
                    fk.targetColumn = row[2] ? row[2] : "";
                    fk.name = row[3] ? row[3] : "";
                    refreshedTable.foreignKeys.push_back(fk);
                }
            }
        }
        while (mysql_next_result(conn) == 0) {
            MYSQL_RES* extra = mysql_store_result(conn);
            if (extra)
                mysql_free_result(extra);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error refreshing table {}: {}", tableName, e.what());
        throw;
    }

    return refreshedTable;
}

bool MySQLDatabaseNode::isTableRefreshing(const std::string& tableName) const {
    auto it = tableRefreshLoaders.find(tableName);
    return it != tableRefreshLoaders.end() && it->second.isRunning();
}

ConnectionPool<MYSQL*>::Session MySQLDatabaseNode::getSession() const {
    if (!connectionPool) {
        throw std::runtime_error(
            "MySQLDatabaseNode::getSession: Connection pool not available for database: " + name);
    }
    return connectionPool->acquire();
}

void MySQLDatabaseNode::initializeConnectionPool(const DatabaseConnectionInfo& info) {
    if (!parentDb) {
        return;
    }

    spdlog::debug("initializeConnectionPool {}", info.buildConnectionString());
    if (connectionPool) {
        return;
    }

    connectionPool = std::make_unique<ConnectionPool<MYSQL*>>(
        makeMysqlFactory(info),
        // closer
        [](MYSQL* conn) { mysql_close(conn); },
        // validator
        [](MYSQL* conn) { return mysql_ping(conn) == 0; });
}

std::vector<std::vector<std::string>>
MySQLDatabaseNode::getTableData(const Table& table, const int limit, const int offset,
                                const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;

    try {
        auto session = getSession();
        MYSQL* conn = session.get();

        const auto builder = createSQLBuilder(getDatabaseType());
        const std::string query =
            builder->selectAll(table, whereClause, orderByClause, limit, offset);

        if (mysql_query(conn, query.c_str()) != 0) {
            spdlog::error("Error getting table data for {}: {}", table.name, mysql_error(conn));
            return result;
        }

        MysqlResPtr res(mysql_store_result(conn));
        if (!res)
            return result;

        unsigned int nFields = mysql_num_fields(res.get());

        // detect boolean columns (TINYINT with length 1)
        MYSQL_FIELD* fields = mysql_fetch_fields(res.get());
        std::vector<bool> isBoolCol(nFields, false);
        for (unsigned int i = 0; i < nFields; i++) {
            isBoolCol[i] = (fields[i].type == MYSQL_TYPE_TINY && fields[i].length == 1);
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get())) != nullptr) {
            unsigned long* lengths = mysql_fetch_lengths(res.get());
            std::vector<std::string> rowData;
            rowData.reserve(nFields);
            for (unsigned int i = 0; i < nFields; i++) {
                if (row[i] == nullptr) {
                    rowData.emplace_back(NULL_SENTINEL);
                } else if (isBoolCol[i]) {
                    rowData.emplace_back(row[i][0] == '1' ? BOOL_TRUE_SENTINEL
                                                          : BOOL_FALSE_SENTINEL);
                } else {
                    rowData.emplace_back(row[i], lengths[i]);
                }
            }
            result.push_back(std::move(rowData));
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting table data for {}: {}", table.name, e.what());
    }

    return result;
}

std::vector<std::string> MySQLDatabaseNode::getColumnNames(const Table& table) {
    std::vector<std::string> columnNames;

    try {
        auto session = getSession();
        MYSQL* conn = session.get();
        const auto builder = createSQLBuilder(getDatabaseType());
        const std::string query = builder->columnNames(table);

        if (mysql_query(conn, query.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    if (row[0])
                        columnNames.emplace_back(row[0]);
                }
            }
        }
        while (mysql_next_result(conn) == 0) {
            MYSQL_RES* extra = mysql_store_result(conn);
            if (extra)
                mysql_free_result(extra);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting column names for {}: {}", table.name, e.what());
    }

    return columnNames;
}

int MySQLDatabaseNode::getRowCount(const Table& table, const std::string& whereClause) {
    int count = 0;

    try {
        auto session = getSession();
        MYSQL* conn = session.get();

        const auto builder = createSQLBuilder(getDatabaseType());
        const std::string query = builder->countRows(table, whereClause);

        if (mysql_query(conn, query.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn));
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res.get());
                if (row && row[0]) {
                    count = std::atoi(row[0]);
                }
            }
        }
        while (mysql_next_result(conn) == 0) {
            MYSQL_RES* extra = mysql_store_result(conn);
            if (extra)
                mysql_free_result(extra);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting row count for {}: {}", table.name, e.what());
    }

    return count;
}

QueryResult MySQLDatabaseNode::executeQuery(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    try {
        auto session = getSession();
        MYSQL* conn = session.get();

        if (mysql_query(conn, query.c_str()) != 0) {
            StatementResult r;
            r.success = false;
            r.errorMessage = mysql_error(conn);
            result.statements.push_back(r);
            return result;
        }

        do {
            auto r = extractMysqlResult(conn, rowLimit);
            if (r.success || !r.errorMessage.empty()) {
                result.statements.push_back(std::move(r));
            }
        } while (mysql_next_result(conn) == 0);
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

std::pair<bool, std::string> MySQLDatabaseNode::createTable(const Table& table) {
    try {
        const auto builder = createSQLBuilder(getDatabaseType());
        std::string sql = builder->createTable(table);

        auto result = executeQuery(sql);
        if (!result.success()) {
            return {false, result.errorMessage()};
        }
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

DatabaseInterface* MySQLDatabaseNode::ownerDatabase() const {
    return parentDb;
}

std::string MySQLDatabaseNode::getFullPath() const {
    return name;
}

DatabaseType MySQLDatabaseNode::getDatabaseType() const {
    if (parentDb) {
        return parentDb->getConnectionInfo().type;
    }
    return DatabaseType::MYSQL;
}

void MySQLDatabaseNode::checkLoadingStatus() {
    checkTablesStatusAsync();
    checkViewsStatusAsync();
    checkRoutinesStatusAsync();
}

std::pair<bool, std::string> MySQLDatabaseNode::renameTable(const std::string& oldName,
                                                            const std::string& newName) {
    auto sql = std::format("RENAME TABLE `{}` TO `{}`", oldName, newName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MySQLDatabaseNode::dropTable(const std::string& tableName) {
    auto sql = std::format("DROP TABLE `{}`", tableName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MySQLDatabaseNode::truncateTable(const std::string& tableName) {
    auto sql = std::format("TRUNCATE TABLE {}", quoteMysqlId(tableName));
    auto r = executeQuery(sql);
    if (r.success())
        return {true, ""};
    return {false, r.errorMessage()};
}

std::pair<bool, std::string> MySQLDatabaseNode::dropColumn(const std::string& tableName,
                                                           const std::string& columnName) {
    auto sql = std::format("ALTER TABLE `{}` DROP COLUMN `{}`", tableName, columnName);
    auto r = executeQuery(sql);
    if (r.success()) {
        startTablesLoadAsync(true);
        return {true, ""};
    }
    return {false, r.errorMessage()};
}
