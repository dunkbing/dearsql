#include "database/file_database.hpp"
#include <dearsql/factory.hpp>

namespace {
    // a catalog call on a loader thread; the error rides back with the result
    template <typename T, typename F>
    LoadResult<T> load(const dearsql::DatabasePtr& db, const char* what, F&& fn) {
        if (!db)
            return {{}, "Database not connected"};
        try {
            return {fn(*db), ""};
        } catch (const std::exception& e) {
            spdlog::error("{}: {}", what, e.what());
            return {{}, e.what()};
        }
    }

    // the sidebar keys tabs on "connection.table"
    void stampFullNames(std::vector<Table>& tables, const std::string& connName) {
        for (auto& t : tables)
            t.fullName = connName + "." + t.name;
    }
} // namespace

std::pair<bool, std::string> FileDatabase::connect() {
    if (connected && handle())
        return {true, ""};
    auto conn = dearsql::makeConnection(connectionInfo);
    if (!conn)
        return {false, "Unsupported database type"};
    auto [ok, err] = conn->open();
    if (!ok) {
        spdlog::error("Can't open {}: {}", connectionInfo.path, err);
        return {false, err};
    }
    {
        std::lock_guard lock(handleMutex_);
        db_ = conn->database();
        conn_ = std::move(conn);
    }
    connected = true;
    spdlog::info("Opened {}", connectionInfo.path);
    return {true, ""};
}

void FileDatabase::disconnect() {
    connected = false;
    std::lock_guard lock(handleMutex_);
    db_.reset();
    if (conn_)
        conn_->close();
    conn_.reset();
}

QueryResult FileDatabase::executeQuery(const std::string& sql, int limit) {
    if (auto db = handle())
        return db->execute(sql, limit);
    QueryResult result;
    result.statements.push_back({.success = false, .errorMessage = "Database not connected"});
    return result;
}

std::pair<bool, std::string> FileDatabase::createTable(const Table& table) {
    auto db = handle();
    return db ? db->createTable(table) : std::pair{false, std::string("Database not connected")};
}

std::vector<std::vector<std::string>> FileDatabase::getTableData(const Table& table, int limit,
                                                                 int offset,
                                                                 const std::string& whereClause,
                                                                 const std::string& orderBy) {
    std::string err;
    auto db = handle();
    if (!db)
        return {};
    return libCall(err, "getTableData",
                   [&] { return db->getTableData(table, limit, offset, whereClause, orderBy); });
}

std::vector<std::string> FileDatabase::getColumnNames(const Table& table) {
    std::string err;
    auto db = handle();
    return db ? libCall(err, "getColumnNames", [&] { return db->getColumnNames(table); })
              : std::vector<std::string>{};
}

int FileDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    std::string err;
    auto db = handle();
    return db ? libCall(err, "getRowCount", [&] { return db->getRowCount(table, whereClause); })
              : 0;
}

LoadResult<Table> FileDatabase::getTablesAsync() {
    auto r = load<Table>(handle(), "load tables", [](auto& db) { return db.tables(); });
    stampFullNames(r.items, connectionInfo.name);
    return r;
}

LoadResult<Table> FileDatabase::getViewsAsync() {
    auto r = load<Table>(handle(), "load views", [](auto& db) { return db.views(); });
    stampFullNames(r.items, connectionInfo.name);
    return r;
}

LoadResult<std::string> FileDatabase::getSequencesAsync() {
    return load<std::string>(handle(), "load sequences", [](auto& db) { return db.sequences(); });
}

void FileDatabase::startTableRefreshAsync(const std::string& tableName) {
    tableRefreshLoaders[tableName].start([this, tableName, db = handle()]() {
        std::string err;
        Table t = db ? libCall(err, "describe table", [&] { return db->describeTable(tableName); })
                     : Table{};
        t.fullName = connectionInfo.name + "." + tableName;
        return t;
    });
}

// any thread (sidebar DDL runs on a worker): checkLoadingStatus starts the reload
std::pair<bool, std::string> FileDatabase::afterDdl(const dearsql::Status& status) {
    if (status.first)
        tablesReloadPending_ = true;
    return status;
}

std::pair<bool, std::string> FileDatabase::renameTable(const std::string& oldName,
                                                       const std::string& newName) {
    auto db = handle();
    return db ? afterDdl(db->renameTable(oldName, newName))
              : std::pair{false, std::string("Database not connected")};
}

std::pair<bool, std::string> FileDatabase::dropTable(const std::string& tableName) {
    auto db = handle();
    return db ? afterDdl(db->dropTable(tableName))
              : std::pair{false, std::string("Database not connected")};
}

std::pair<bool, std::string> FileDatabase::dropColumn(const std::string& tableName,
                                                      const std::string& columnName) {
    auto db = handle();
    return db ? afterDdl(db->dropColumn(tableName, columnName))
              : std::pair{false, std::string("Database not connected")};
}
