#pragma once

#include "async_helper.hpp"
#include "database_node.hpp"
#include "db_interface.hpp"
#include "query_executor.hpp"
#include <dearsql/factory.hpp>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>

// a database server (Postgres, MySQL, MSSQL, ...) whose work is done by a
// libdearsql IConnection. this adds what the library leaves to the host: the ssh
// tunnel, async connect / database-list / refresh, and the cache of per-database
// nodes the sidebar renders. backends supply makeNode(); the rest is shared.
template <typename NodeT> class ServerDatabase : public DatabaseInterface, public IQueryExecutor {
public:
    using NodeMap = std::unordered_map<std::string, std::unique_ptr<NodeT>>;

    explicit ServerDatabase(const DatabaseConnectionInfo& info) {
        setConnectionInfo(info);
    }

    ~ServerDatabase() override {
        // join every worker while the members they touch still exist
        connectionOp.wait();
        refreshWorkflow.wait();
        databasesLoader.wait();
        for (auto& node : nodes_ | std::views::values)
            node->waitForLoaders();
        nodes_.clear();
        ServerDatabase::disconnect();
    }

    std::pair<bool, std::string> connect() override {
        if (connected)
            return {true, ""};
        setAttemptedConnection(true);
        auto [ok, err] = openConnection();
        setLastConnectionError(err);
        if (!ok)
            return {false, err};
        // the database list starts from the UI thread (checkConnectionStatusAsync):
        // connect() itself runs on a worker
        return {true, ""};
    }

    void disconnect() override {
        for (auto& node : nodes_ | std::views::values)
            node->resetPool();
        closeConnection();
    }

    void checkConnectionStatusAsync() override {
        DatabaseInterface::checkConnectionStatusAsync();
        if (connected && listsAllDatabases() && !databasesLoaded && !databasesLoader.isRunning())
            refreshDatabaseNames();
    }

    // nodes are reset here, on the UI thread; the worker only reconnects and
    // re-lists, and checkRefreshWorkflowAsync picks up the result
    void refreshConnection() override {
        if (refreshWorkflow.isRunning())
            return;
        for (auto& node : nodes_ | std::views::values) {
            node->cancelLoaders();
            node->resetPool();
        }
        refreshWorkflow.start([this]() -> bool {
            closeConnection();
            setLastConnectionError("");
            auto [ok, err] = openConnection();
            if (!ok) {
                setLastConnectionError(err);
                return false;
            }
            auto listed = loadNames();
            std::lock_guard lock(refreshMutex_);
            pendingNames_ = std::move(listed);
            return true;
        });
    }

    [[nodiscard]] bool isConnecting() const override {
        return connectionOp.isRunning() || refreshWorkflow.isRunning();
    }

    // server-level queries run on the default database's node
    QueryResult executeQuery(const std::string& query, int rowLimit = 1000) override {
        if (!connect().first) {
            QueryResult r;
            r.statements.push_back({.success = false, .errorMessage = "Not connected"});
            return r;
        }
        return getDatabaseData(connectionInfo.database)->executeQuery(query, rowLimit);
    }

    std::pair<bool, std::string>
    createDatabaseWithOptions(const CreateDatabaseOptions& options) override {
        auto conn = connection();
        if (!conn)
            return {false, "Not connected to database"};
        if (options.name.empty())
            return {false, "Database name cannot be empty"};
        // may run on a worker: the caller relists on the UI thread
        return conn->createDatabase(options);
    }

    std::pair<bool, std::string> createDatabase(const std::string& name,
                                                const std::string& comment = "") override {
        CreateDatabaseOptions options;
        options.name = name;
        options.comment = comment;
        return createDatabaseWithOptions(options);
    }

    std::pair<bool, std::string> dropDatabase(const std::string& name) override {
        auto ddl = beginDropDatabase(name);
        auto status = ddl.work();
        if (ddl.finish)
            ddl.finish(status.first);
        return status;
    }

    std::pair<bool, std::string> renameDatabase(const std::string& oldName,
                                                const std::string& newName) override {
        auto ddl = beginRenameDatabase(oldName, newName);
        auto status = ddl.work();
        if (ddl.finish)
            ddl.finish(status.first);
        return status;
    }

    DatabaseDdl beginDropDatabase(const std::string& name) override {
        auto conn = connection();
        if (!conn)
            return notConnected();
        auto suspended = suspendNode(name);
        NodeT* node = suspended.first;
        std::shared_ptr<void> pin = std::move(suspended.second);
        auto fallback = std::make_shared<std::string>();
        DatabaseDdl ddl;
        ddl.work = [conn, node, pin, name, fallback]() mutable {
            // no pooled session may block DROP: cancel the busy ones and wait them out
            if (node)
                node->resetPool(true);
            auto status = conn->dropDatabase(name);
            *fallback = conn->info().database;
            pin.reset();
            return status;
        };
        ddl.finish = [this, name, fallback](bool ok) {
            if (!ok) {
                resumeNode(name);
                return;
            }
            // dropped the one we were on: follow the library to its fallback, or none
            if (name == connectionInfo.database)
                connectionInfo.database = databaseAfterDrop(name, *fallback);
            retireDatabaseNode(name);
            refreshDatabaseNames();
        };
        return ddl;
    }

    DatabaseDdl beginRenameDatabase(const std::string& oldName,
                                    const std::string& newName) override {
        auto conn = connection();
        if (!conn)
            return notConnected();
        auto suspended = suspendNode(oldName);
        NodeT* node = suspended.first;
        std::shared_ptr<void> pin = std::move(suspended.second);
        DatabaseDdl ddl;
        ddl.work = [conn, node, pin, oldName, newName]() mutable {
            if (node)
                node->resetPool(true);
            auto status = conn->renameDatabase(oldName, newName);
            pin.reset();
            return status;
        };
        ddl.finish = [this, oldName](bool ok) {
            if (!ok) {
                resumeNode(oldName);
                return;
            }
            retireDatabaseNode(oldName);
            refreshDatabaseNames();
        };
        return ddl;
    }

    void refreshDatabaseNames() override {
        // a list already loading may predate the change that asked for this one
        if (databasesLoader.isRunning()) {
            relistPending_ = true;
            return;
        }
        databasesLoaded = false;
        databasesLoader.start([this] { return loadNames(); });
    }

    [[nodiscard]] bool areDatabasesLoaded() const {
        return databasesLoaded;
    }
    [[nodiscard]] bool isLoadingDatabases() const {
        return databasesLoader.isRunning();
    }
    [[nodiscard]] const std::string& getLastDatabasesError() const {
        return lastDatabasesError_;
    }

    void checkDatabasesStatusAsync() {
        databasesLoader.check([this](const NameList& listed) {
            lastDatabasesError_ = listed.error;
            for (const auto& name : listed.names)
                getDatabaseData(name);
            databasesLoaded = true;
        });
        if (relistPending_ && !databasesLoader.isRunning()) {
            relistPending_ = false;
            refreshDatabaseNames();
        }
    }

    void checkRefreshWorkflowAsync() {
        refreshWorkflow.check([this](bool ok) {
            if (!ok)
                return;
            NameList listed;
            {
                std::lock_guard lock(refreshMutex_);
                listed = std::move(pendingNames_);
            }
            lastDatabasesError_ = listed.error;
            for (const auto& name : listed.names)
                getDatabaseData(name);
            databasesLoaded = true;
            for (auto& node : nodes_ | std::views::values)
                onRefreshed(*node);
        });
    }

    [[nodiscard]] bool hasPendingAsyncWork() const override {
        if (isConnecting() || isLoadingDatabases())
            return true;
        return std::ranges::any_of(nodes_ | std::views::values,
                                   [](const auto& node) { return node->hasPendingAsyncWork(); });
    }

    // node for a database, created on first use
    NodeT* getDatabaseData(const std::string& name) {
        auto it = nodes_.find(name);
        if (it == nodes_.end())
            it = nodes_.emplace(name, makeNode(name)).first;
        return it->second.get();
    }

    // every known database; lists them on first access
    NodeMap& getDatabaseDataMap() {
        if (!databasesLoaded && !databasesLoader.isRunning() && isConnected())
            refreshDatabaseNames();
        return nodes_;
    }
    const NodeMap& getDatabaseDataMap() const {
        return nodes_;
    }

    // the library connection (null when disconnected); nodes open their handles here
    [[nodiscard]] std::shared_ptr<dearsql::IConnection> connection() const {
        std::lock_guard lock(connMutex_);
        return conn_;
    }
    [[nodiscard]] std::shared_ptr<dearsql::IConnection> libConnection() const override {
        return connection();
    }

protected:
    virtual std::unique_ptr<NodeT> makeNode(const std::string& name) = 0;

    // databases shown in the sidebar; runs on a worker, may throw
    virtual std::vector<std::string> listDatabaseNames() {
        if (!listsAllDatabases())
            return {connectionInfo.database};
        auto conn = connection();
        if (!conn)
            return {};
        std::vector<std::string> names;
        for (const auto& db : conn->databases())
            names.push_back(db->name());
        return names;
    }

    // after a refresh; default reloads tables and views
    virtual void onRefreshed(NodeT& node) {
        node.startTablesLoadAsync(true);
        node.startViewsLoadAsync(true);
    }

    // where the connection lands after dropping the database it was on;
    // libFallback is where the library moved its own connection
    virtual std::string databaseAfterDrop(const std::string& dropped,
                                          const std::string& libFallback) {
        return libFallback != dropped ? libFallback : "";
    }

    [[nodiscard]] bool listsAllDatabases() const {
        return connectionInfo.showAllDatabases || connectionInfo.database.empty();
    }

    // names with the error, so the worker never writes state the UI reads
    struct NameList {
        std::vector<std::string> names;
        std::string error;
    };
    AsyncOperation<NameList> databasesLoader;
    AsyncOperation<bool> refreshWorkflow;
    bool databasesLoaded = false;

private:
    NameList loadNames() {
        try {
            return {listDatabaseNames(), ""};
        } catch (const std::exception& e) {
            spdlog::error("list databases: {}", e.what());
            return {{}, e.what()};
        }
    }

    static DatabaseDdl notConnected() {
        return {[] { return std::pair<bool, std::string>{false, "Not connected to database"}; },
                nullptr};
    }

    // UI thread: no loader may reopen a session on the database while a worker
    // drops or renames it; the pin keeps the node alive for that worker
    std::pair<NodeT*, std::shared_ptr<void>> suspendNode(const std::string& name) {
        auto it = nodes_.find(name);
        if (it == nodes_.end())
            return {nullptr, nullptr};
        NodeT* node = it->second.get();
        node->setSuspended(true);
        node->abandonLoaders();
        return {node, node->pin()};
    }

    void resumeNode(const std::string& name) {
        if (auto it = nodes_.find(name); it != nodes_.end())
            it->second->setSuspended(false);
    }

    // out of the map first, then to the app, which closes tabs on it before it dies
    void retireDatabaseNode(const std::string& name) {
        auto it = nodes_.find(name);
        if (it == nodes_.end())
            return;
        std::unique_ptr<IDatabaseNode> node = std::move(it->second);
        nodes_.erase(it);
        retireNode(std::move(node));
    }

    // library connection and tunnel only; nodes are the caller's business
    void closeConnection() {
        {
            std::lock_guard lock(connMutex_);
            if (conn_)
                conn_->close();
            conn_.reset();
        }
        stopSshTunnel();
        connected = false;
    }

    // ssh tunnel, then the library connection
    std::pair<bool, std::string> openConnection() {
        auto [prepOk, prepErr] = prepareConnectionForConnect();
        if (!prepOk)
            return {false, prepErr};
        auto conn = dearsql::makeConnection(connectionInfo);
        if (!conn)
            return {false, "Unsupported database type"};
        auto [ok, err] = conn->open();
        if (!ok) {
            spdlog::error("{} connection failed: {}", databaseTypeToString(connectionInfo.type),
                          err);
            stopSshTunnel();
            return {false, err};
        }
        {
            std::lock_guard lock(connMutex_);
            conn_ = std::move(conn);
        }
        connected = true;
        return {true, ""};
    }

    NodeMap nodes_;
    std::shared_ptr<dearsql::IConnection> conn_;
    mutable std::mutex connMutex_;
    std::mutex refreshMutex_;
    NameList pendingNames_;
    std::string lastDatabasesError_;
    bool relistPending_ = false;
};
