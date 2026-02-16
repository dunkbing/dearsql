#include "database/mongodb.hpp"
#include "utils/logger.hpp"
#include <format>
#include <ranges>

mongocxx::instance& MongoDBDatabase::getDriverInstance() {
    static mongocxx::instance instance{};
    return instance;
}

MongoDBDatabase::MongoDBDatabase(const DatabaseConnectionInfo& connInfo) {
    // Ensure driver is initialized
    getDriverInstance();

    this->connectionInfo = connInfo;
    if (connectionInfo.port == 0 || connectionInfo.port == 5432) {
        connectionInfo.port = 27017; // Default MongoDB port
    }
    Logger::debug(
        std::format("Creating MongoDBDatabase with host = '{}', port = {}, showAllDatabases = {}",
                    connectionInfo.host, connectionInfo.port, connInfo.showAllDatabases));
}

MongoDBDatabase::~MongoDBDatabase() {
    databasesLoader.cancel();
    refreshWorkflow.cancel();

    for (auto& dbDataPtr : databaseDataCache | std::views::values) {
        if (dbDataPtr) {
            dbDataPtr->collectionsLoader.cancel();
        }
    }

    disconnect();
}

MongoDBDatabaseNode* MongoDBDatabase::getDatabaseData(const std::string& dbName) {
    const auto it = databaseDataCache.find(dbName);
    if (it == databaseDataCache.end()) {
        auto newData = std::make_unique<MongoDBDatabaseNode>();
        newData->name = dbName;
        newData->parentDb = this;
        auto* ptr = newData.get();
        databaseDataCache[dbName] = std::move(newData);
        return ptr;
    }
    return it->second.get();
}

std::pair<bool, std::string> MongoDBDatabase::connect() {
    if (connected) {
        return {true, ""};
    }

    setAttemptedConnection(true);

    try {
        std::string uri = connectionInfo.buildConnectionString();
        Logger::debug(std::format("Connecting to MongoDB: {}", uri));

        std::lock_guard lock(poolMutex);
        connectionPool = std::make_unique<mongocxx::pool>(mongocxx::uri{uri});

        // Test connection by getting a client and listing databases
        const auto client = connectionPool->acquire();
        auto databases = client->list_database_names();

        Logger::info(std::format("Successfully connected to MongoDB at {}:{}", connectionInfo.host,
                                 connectionInfo.port));
        connected = true;
        setLastConnectionError("");

        // Start loading databases if showAllDatabases is enabled
        if (connectionInfo.showAllDatabases && !databasesLoaded && !databasesLoader.isRunning()) {
            Logger::debug("Starting async database loading after connection...");
            refreshDatabaseNames();
        }

        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("MongoDB connection failed: {}", e.what()));
        std::lock_guard lock(poolMutex);
        connectionPool.reset();
        connected = false;
        std::string error = "MongoDB connection failed: " + std::string(e.what());
        setLastConnectionError(error);
        return {false, error};
    }
}

void MongoDBDatabase::disconnect() {
    std::lock_guard lock(poolMutex);
    connectionPool.reset();
    connected = false;
}

void MongoDBDatabase::refreshConnection() {
    refreshWorkflow.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        auto [success, error] = connect();
        if (!success) {
            setLastConnectionError(error);
            return false;
        }

        if (connectionInfo.showAllDatabases) {
            Logger::debug("Loading database names synchronously for refresh...");
            auto databases = getDatabaseNamesAsync();

            for (const auto& dbName : databases) {
                getDatabaseData(dbName);
            }
        }

        databasesLoaded = true;

        // Trigger refresh for all child databases
        Logger::debug("Triggering child database refresh...");
        for (auto& dbDataPtr : databaseDataCache | std::views::values) {
            if (dbDataPtr) {
                Logger::debug(std::format("Refreshing db: {}", dbDataPtr->name));
                dbDataPtr->startCollectionsLoadAsync(true);
            }
        }

        Logger::info(std::format("MongoDB refresh workflow completed for {} databases",
                                 databaseDataCache.size()));
        return true;
    });
}

std::vector<QueryResult> MongoDBDatabase::executeQueryWithResult(const std::string& query,
                                                                 int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!connect().first) {
        result.success = false;
        result.errorMessage = "Not connected to database";
        return {result};
    }

    try {
        // Parse JSON query - expected format:
        // { "database": "db", "collection": "coll", "command": "find", "filter": {} }
        auto doc = bsoncxx::from_json(query);
        auto view = doc.view();

        std::string dbName = connectionInfo.database;
        std::string collName;
        std::string command = "find";

        if (view["database"]) {
            dbName = std::string(view["database"].get_string().value);
        }
        if (view["collection"]) {
            collName = std::string(view["collection"].get_string().value);
        }
        if (view["command"]) {
            command = std::string(view["command"].get_string().value);
        }

        auto client = getClient();
        auto db = (*client)[dbName];

        if (command == "find" && !collName.empty()) {
            auto coll = db[collName];

            bsoncxx::document::view_or_value filter = bsoncxx::builder::stream::document{}
                                                      << bsoncxx::builder::stream::finalize;
            if (view["filter"]) {
                filter = view["filter"].get_document().value;
            }

            mongocxx::options::find opts;
            opts.limit(rowLimit);

            auto cursor = coll.find(filter, opts);

            // Build result from cursor
            result.columnNames.push_back("_id");
            result.columnNames.push_back("document");

            for (auto&& doc : cursor) {
                std::vector<std::string> row;
                if (doc["_id"]) {
                    auto idDoc = bsoncxx::builder::stream::document{}
                                 << "_id" << doc["_id"].get_value()
                                 << bsoncxx::builder::stream::finalize;
                    row.push_back(bsoncxx::to_json(idDoc.view()));
                } else {
                    row.push_back("");
                }
                row.push_back(bsoncxx::to_json(doc));
                result.tableData.push_back(std::move(row));
            }

            result.message = std::format("Returned {} document{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
        } else if (command == "aggregate" && !collName.empty()) {
            auto coll = db[collName];

            mongocxx::pipeline pipeline;
            if (view["pipeline"]) {
                for (auto&& stage : view["pipeline"].get_array().value) {
                    pipeline.append_stage(stage.get_document().value);
                }
            }

            auto cursor = coll.aggregate(pipeline);

            result.columnNames.push_back("document");
            for (auto&& doc : cursor) {
                std::vector<std::string> row;
                row.push_back(bsoncxx::to_json(doc));
                result.tableData.push_back(std::move(row));
            }

            result.message = std::format("Returned {} document{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
        } else if (command == "runCommand") {
            // Run arbitrary command
            if (view["commandDoc"]) {
                auto cmdResult = db.run_command(view["commandDoc"].get_document().value);
                result.columnNames.push_back("result");
                std::vector<std::string> row;
                row.push_back(bsoncxx::to_json(cmdResult.view()));
                result.tableData.push_back(std::move(row));
                result.message = "Command executed successfully";
            }
        } else {
            result.success = false;
            result.errorMessage = "Unknown command or missing collection name";
            return {result};
        }

        result.success = true;
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    return {result};
}

std::pair<bool, std::string> MongoDBDatabase::executeQuery(const std::string& query) {
    if (!connect().first) {
        return {false, "Not connected to database"};
    }

    try {
        auto doc = bsoncxx::from_json(query);
        auto view = doc.view();

        std::string dbName = connectionInfo.database;
        std::string collName;
        std::string command;

        if (view["database"]) {
            dbName = std::string(view["database"].get_string().value);
        }
        if (view["collection"]) {
            collName = std::string(view["collection"].get_string().value);
        }
        if (view["command"]) {
            command = std::string(view["command"].get_string().value);
        }

        auto client = getClient();
        auto db = (*client)[dbName];

        if (command == "insert" && !collName.empty()) {
            auto coll = db[collName];
            if (view["document"]) {
                coll.insert_one(view["document"].get_document().value);
            } else if (view["documents"]) {
                std::vector<bsoncxx::document::view> docs;
                for (auto&& d : view["documents"].get_array().value) {
                    docs.push_back(d.get_document().value);
                }
                coll.insert_many(docs);
            }
            return {true, ""};
        } else if (command == "update" && !collName.empty()) {
            auto coll = db[collName];
            auto filter = view["filter"].get_document().value;
            auto update = view["update"].get_document().value;
            coll.update_many(filter, update);
            return {true, ""};
        } else if (command == "delete" && !collName.empty()) {
            auto coll = db[collName];
            auto filter = view["filter"].get_document().value;
            coll.delete_many(filter);
            return {true, ""};
        } else if (command == "createCollection" && !collName.empty()) {
            db.create_collection(collName);
            return {true, ""};
        } else if (command == "dropCollection" && !collName.empty()) {
            db[collName].drop();
            return {true, ""};
        } else if (command == "runCommand" && view["commandDoc"]) {
            db.run_command(view["commandDoc"].get_document().value);
            return {true, ""};
        }

        return {false, "Unknown command"};
    } catch (const std::exception& e) {
        return {false, std::string(e.what())};
    }
}

std::unordered_map<std::string, std::unique_ptr<MongoDBDatabaseNode>>&
MongoDBDatabase::getDatabaseDataMap() {
    if (!databasesLoaded && !databasesLoader.isRunning() && isConnected()) {
        refreshDatabaseNames();
    }
    return databaseDataCache;
}

void MongoDBDatabase::refreshDatabaseNames() {
    if (databasesLoader.isRunning()) {
        return;
    }

    databasesLoaded = false;
    databasesLoader.start([this]() { return getDatabaseNamesAsync(); });
}

bool MongoDBDatabase::isLoadingDatabases() const {
    return databasesLoader.isRunning();
}

bool MongoDBDatabase::hasPendingAsyncWork() const {
    if (isConnecting() || isLoadingDatabases()) {
        return true;
    }

    for (const auto& [_, dbNode] : databaseDataCache) {
        if (!dbNode) {
            continue;
        }

        if (dbNode->collectionsLoader.isRunning()) {
            return true;
        }
    }

    return false;
}

void MongoDBDatabase::checkDatabasesStatusAsync() {
    databasesLoader.check([this](const std::vector<std::string>& databases) {
        Logger::debug(
            std::format("Async database loading completed. Found {} databases.", databases.size()));

        for (const auto& dbName : databases) {
            getDatabaseData(dbName);
        }

        databasesLoaded = true;
    });
}

void MongoDBDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow.check([](const bool success) {
        if (success) {
            Logger::info("MongoDB refresh workflow completed successfully");
        } else {
            Logger::error("MongoDB refresh workflow failed");
        }
    });
}

std::vector<std::string> MongoDBDatabase::getDatabaseNamesAsync() const {
    Logger::info("MongoDBDatabase::getDatabaseNamesAsync");
    std::vector<std::string> result;

    try {
        if (!isConnected()) {
            Logger::error("Cannot load databases: not connected");
            return result;
        }

        if (!connectionInfo.showAllDatabases) {
            if (!connectionInfo.database.empty()) {
                result.push_back(connectionInfo.database);
            }
            return result;
        }

        auto client = getClient();
        auto databases = client->list_database_names();

        for (const auto& dbName : databases) {
            // Filter out system databases
            if (dbName != "admin" && dbName != "config" && dbName != "local") {
                result.push_back(dbName);
            }
        }
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to list databases: {}", e.what()));
    }

    Logger::debug(std::format("Found {} databases", result.size()));
    return result;
}

std::pair<bool, std::string> MongoDBDatabase::dropDatabase(const std::string& dbName) {
    if (!isConnected()) {
        return {false, "Not connected to database"};
    }

    try {
        auto client = getClient();
        (*client)[dbName].drop();

        databaseDataCache.erase(dbName);

        Logger::info(std::format("Database '{}' dropped successfully", dbName));
        return {true, ""};
    } catch (const std::exception& e) {
        Logger::error(std::format("Failed to drop database: {}", e.what()));
        return {false, e.what()};
    }
}

mongocxx::pool::entry MongoDBDatabase::getClient() const {
    std::lock_guard lock(poolMutex);
    if (!connectionPool) {
        throw std::runtime_error("MongoDBDatabase::getClient: Connection pool not available");
    }
    return connectionPool->acquire();
}
