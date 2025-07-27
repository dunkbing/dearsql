#include "database/redis.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <sstream>

RedisDatabase::RedisDatabase(std::string name, std::string host, int port, std::string password)
    : name(std::move(name)), host(std::move(host)), port(port), password(std::move(password)) {
    connectionString = std::format("redis://{}:{}", this->host, this->port);
}

RedisDatabase::~RedisDatabase() {
    disconnect();
}

std::pair<bool, std::string> RedisDatabase::connect() {
    if (connected && context) {
        return {true, ""};
    }

    attemptedConnection = true;
    std::cout << "Attempting Redis connection to " << host << ":" << port << std::endl;

    try {
        // Use redisConnectWithTimeout for better timeout handling
        constexpr timeval timeout = {5, 0}; // 5 seconds timeout
        context = redisConnectWithTimeout(host.c_str(), port, timeout);
        if (!context || context->err) {
            std::string error = context ? context->errstr : "Failed to allocate redis context";
            lastConnectionError = error;
            std::cout << "Redis connection failed: " << error << std::endl;
            if (context) {
                redisFree(context);
                context = nullptr;
            }
            return {false, error};
        }

        // Authenticate if password is provided
        if (!password.empty()) {
            auto *reply = (redisReply *)redisCommand(context, "AUTH %s", password.c_str());
            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                std::string error = reply ? reply->str : "Authentication failed";
                lastConnectionError = error;
                if (reply)
                    freeReplyObject(reply);
                redisFree(context);
                context = nullptr;
                return {false, error};
            }
            freeReplyObject(reply);
        }

        // Test connection with PING
        auto *reply = (redisReply *)redisCommand(context, "PING");
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            std::string error = reply ? reply->str : "Connection test failed";
            lastConnectionError = error;
            if (reply)
                freeReplyObject(reply);
            redisFree(context);
            context = nullptr;
            return {false, error};
        }
        freeReplyObject(reply);

        connected = true;
        lastConnectionError.clear();
        std::cout << "Successfully connected to Redis: " << connectionString << std::endl;
        return {true, ""};
    } catch (const std::exception &e) {
        std::string error = e.what();
        lastConnectionError = error;
        if (context) {
            redisFree(context);
            context = nullptr;
        }
        return {false, error};
    }
}

void RedisDatabase::disconnect() {
    if (context) {
        redisFree(context);
        context = nullptr;
    }
    connected = false;
}

bool RedisDatabase::isConnected() const {
    return connected && context;
}

bool RedisDatabase::isConnecting() const {
    return connecting;
}

void RedisDatabase::startConnectionAsync() {
    if (connecting || connected) {
        return;
    }

    connecting = true;
    connectionFuture = std::async(std::launch::async, [this]() { return connect(); });
}

void RedisDatabase::checkConnectionStatusAsync() {
    if (!connecting) {
        return;
    }

    if (connectionFuture.valid() &&
        connectionFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = connectionFuture.get();
            connecting = false;
            // Connection result is already stored in the connect() method
        } catch (const std::exception &e) {
            connecting = false;
            lastConnectionError = e.what();
        }
    }
}

const std::string &RedisDatabase::getName() const {
    return name;
}

const std::string &RedisDatabase::getConnectionString() const {
    return connectionString;
}

const std::string &RedisDatabase::getPath() const {
    return connectionString;
}

void *RedisDatabase::getConnection() const {
    return context;
}

DatabaseType RedisDatabase::getType() const {
    return DatabaseType::REDIS;
}

void RedisDatabase::refreshTables() {
    if (!isConnected()) {
        tablesLoaded = true;
        return;
    }

    tables.clear();
    groupKeysByPattern();
    tablesLoaded = true;
}

bool RedisDatabase::isLoadingTables() const {
    return loadingTables;
}

void RedisDatabase::checkTablesStatusAsync() {
    if (tablesFuture.valid() &&
        tablesFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tablesFuture.get();
            loadingTables = false;
            tablesLoaded = true;
        } catch (const std::exception &e) {
            std::cerr << "Error loading Redis keys: " << e.what() << std::endl;
            loadingTables = false;
            tablesLoaded = true;
        }
    }
}

const std::vector<Table> &RedisDatabase::getTables() const {
    return tables;
}

std::vector<Table> &RedisDatabase::getTables() {
    return tables;
}

bool RedisDatabase::areTablesLoaded() const {
    return tablesLoaded;
}

void RedisDatabase::setTablesLoaded(bool loaded) {
    tablesLoaded = loaded;
}

// Views not applicable for Redis
void RedisDatabase::refreshViews() {
    views.clear();
    viewsLoaded = true;
}

const std::vector<Table> &RedisDatabase::getViews() const {
    return views;
}

std::vector<Table> &RedisDatabase::getViews() {
    return views;
}

bool RedisDatabase::areViewsLoaded() const {
    return viewsLoaded;
}

void RedisDatabase::setViewsLoaded(bool loaded) {
    viewsLoaded = loaded;
}

// Sequences not applicable for Redis
void RedisDatabase::refreshSequences() {
    sequences.clear();
    sequencesLoaded = true;
}

const std::vector<std::string> &RedisDatabase::getSequences() const {
    return sequences;
}

std::vector<std::string> &RedisDatabase::getSequences() {
    return sequences;
}

bool RedisDatabase::areSequencesLoaded() const {
    return sequencesLoaded;
}

void RedisDatabase::setSequencesLoaded(const bool loaded) {
    sequencesLoaded = loaded;
}

std::string RedisDatabase::executeQuery(const std::string &command) {
    if (!isConnected()) {
        return "Error: Not connected to Redis server";
    }

    try {
        redisReply *reply = executeRedisCommand(command);
        if (!reply) {
            return "Error: Failed to execute command";
        }

        std::string result = formatRedisReply(reply);
        freeReplyObject(reply);
        return result;
    } catch (const std::exception &e) {
        return "Error: " + std::string(e.what());
    }
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
RedisDatabase::executeQueryStructured(const std::string &command) {
    std::vector<std::string> columnNames = {"Result"};
    std::vector<std::vector<std::string>> data;

    if (!isConnected()) {
        data.push_back({"Error: Not connected to Redis server"});
        return {columnNames, data};
    }

    try {
        redisReply *reply = executeRedisCommand(command);
        if (!reply) {
            data.push_back({"Error: Failed to execute command"});
            return {columnNames, data};
        }

        // Handle different reply types
        if (reply->type == REDIS_REPLY_ARRAY) {
            columnNames = {"Index", "Value"};
            for (size_t i = 0; i < reply->elements; ++i) {
                std::vector<std::string> row;
                row.push_back(std::to_string(i));
                row.push_back(formatRedisReply(reply->element[i]));
                data.push_back(row);
            }
        } else {
            data.push_back({formatRedisReply(reply)});
        }

        freeReplyObject(reply);
        return {columnNames, data};
    } catch (const std::exception &e) {
        data.push_back({"Error: " + std::string(e.what())});
        return {columnNames, data};
    }
}

std::vector<std::vector<std::string>> RedisDatabase::getTableData(const std::string &keyPattern,
                                                                  int limit, int offset) {
    std::vector<std::vector<std::string>> data;

    if (!isConnected()) {
        return data;
    }

    try {
        auto keys = getKeys(keyPattern, limit + offset);

        // Apply offset
        if (offset >= static_cast<int>(keys.size())) {
            return data;
        }

        auto startIt = keys.begin() + offset;
        auto endIt = keys.begin() + std::min(offset + limit, static_cast<int>(keys.size()));

        for (auto it = startIt; it != endIt; ++it) {
            std::vector<std::string> row;
            row.push_back(it->name);
            row.push_back(it->type);
            row.push_back(it->value);
            row.push_back(it->ttl == -1 ? "No expiration" : std::to_string(it->ttl) + "s");
            data.push_back(row);
        }
    } catch (const std::exception &e) {
        std::cerr << "Error getting Redis key data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> RedisDatabase::getColumnNames(const std::string &keyPattern) {
    return {"Key", "Type", "Value", "TTL"};
}

int RedisDatabase::getRowCount(const std::string &keyPattern) {
    if (!isConnected()) {
        return 0;
    }

    try {
        auto keys = getKeys(keyPattern, 10000); // Get up to 10k keys for count
        return static_cast<int>(keys.size());
    } catch (const std::exception &e) {
        std::cerr << "Error getting Redis key count: " << e.what() << std::endl;
        return 0;
    }
}

// Async table data loading methods
void RedisDatabase::startTableDataLoadAsync(const std::string &keyPattern, int limit, int offset) {
    if (loadingTableData) {
        return;
    }

    loadingTableData = true;
    hasTableDataReady = false;
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;

    tableDataFuture = std::async(std::launch::async, [this, keyPattern, limit, offset]() {
        try {
            tableDataResult = getTableData(keyPattern, limit, offset);
            columnNamesResult = getColumnNames(keyPattern);
            rowCountResult = getRowCount(keyPattern);
        } catch (const std::exception &e) {
            std::cerr << "Error in async Redis data load: " << e.what() << std::endl;
            tableDataResult.clear();
            columnNamesResult.clear();
            rowCountResult = 0;
        }
    });
}

bool RedisDatabase::isLoadingTableData() const {
    return loadingTableData;
}

void RedisDatabase::checkTableDataStatusAsync() {
    if (!loadingTableData) {
        return;
    }

    if (tableDataFuture.valid() &&
        tableDataFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tableDataFuture.get();
            hasTableDataReady = true;
            loadingTableData = false;
        } catch (const std::exception &e) {
            std::cerr << "Error loading Redis data: " << e.what() << std::endl;
            loadingTableData = false;
            hasTableDataReady = false;
            tableDataResult.clear();
            columnNamesResult.clear();
            rowCountResult = 0;
        }
    }
}

bool RedisDatabase::hasTableDataResult() const {
    return hasTableDataReady;
}

std::vector<std::vector<std::string>> RedisDatabase::getTableDataResult() {
    if (hasTableDataReady) {
        return tableDataResult;
    }
    return {};
}

std::vector<std::string> RedisDatabase::getColumnNamesResult() {
    if (hasTableDataReady) {
        return columnNamesResult;
    }
    return {};
}

int RedisDatabase::getRowCountResult() {
    if (hasTableDataReady) {
        return rowCountResult;
    }
    return 0;
}

void RedisDatabase::clearTableDataResult() {
    hasTableDataReady = false;
    tableDataResult.clear();
    columnNamesResult.clear();
    rowCountResult = 0;
}

bool RedisDatabase::isExpanded() const {
    return expanded;
}

void RedisDatabase::setExpanded(bool exp) {
    expanded = exp;
}

bool RedisDatabase::hasAttemptedConnection() const {
    return attemptedConnection;
}

void RedisDatabase::setAttemptedConnection(bool attempted) {
    attemptedConnection = attempted;
}

const std::string &RedisDatabase::getLastConnectionError() const {
    return lastConnectionError;
}

void RedisDatabase::setLastConnectionError(const std::string &error) {
    lastConnectionError = error;
}

// Redis-specific methods
std::vector<RedisKey> RedisDatabase::getKeys(const std::string &pattern, int limit) {
    std::vector<RedisKey> keys;

    if (!isConnected()) {
        return keys;
    }

    try {
        auto *reply = (redisReply *)redisCommand(context, "KEYS %s", pattern.c_str());
        if (!reply || reply->type != REDIS_REPLY_ARRAY) {
            if (reply)
                freeReplyObject(reply);
            return keys;
        }

        const int count = std::min(limit, static_cast<int>(reply->elements));
        for (int i = 0; i < count; ++i) {
            if (reply->element[i]->type == REDIS_REPLY_STRING) {
                RedisKey key;
                key.name = reply->element[i]->str;
                key.type = getKeyType(key.name);
                key.value = getKeyValue(key.name);
                key.ttl = getKeyTTL(key.name);
                keys.push_back(key);
            }
        }

        freeReplyObject(reply);
    } catch (const std::exception &e) {
        std::cerr << "Error getting Redis keys: " << e.what() << std::endl;
    }

    return keys;
}

std::string RedisDatabase::getKeyValue(const std::string &key) {
    if (!isConnected()) {
        return "";
    }

    try {
        std::string type = getKeyType(key);

        if (type == "string") {
            auto *reply = (redisReply *)redisCommand(context, "GET %s", key.c_str());
            if (reply && reply->type == REDIS_REPLY_STRING) {
                std::string value = reply->str;
                freeReplyObject(reply);
                return value;
            }
            if (reply)
                freeReplyObject(reply);
        } else if (type == "list") {
            auto *reply = (redisReply *)redisCommand(context, "LRANGE %s 0 4", key.c_str());
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                std::stringstream ss;
                ss << "[";
                for (size_t i = 0; i < reply->elements && i < 5; ++i) {
                    if (i > 0)
                        ss << ", ";
                    ss << "\"" << reply->element[i]->str << "\"";
                }
                if (reply->elements > 5)
                    ss << ", ...";
                ss << "]";
                std::string value = ss.str();
                freeReplyObject(reply);
                return value;
            }
            if (reply)
                freeReplyObject(reply);
        } else if (type == "set") {
            auto *reply = (redisReply *)redisCommand(context, "SMEMBERS %s", key.c_str());
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                std::stringstream ss;
                ss << "{";
                for (size_t i = 0; i < reply->elements && i < 5; ++i) {
                    if (i > 0)
                        ss << ", ";
                    ss << "\"" << reply->element[i]->str << "\"";
                }
                if (reply->elements > 5)
                    ss << ", ...";
                ss << "}";
                std::string value = ss.str();
                freeReplyObject(reply);
                return value;
            }
            if (reply)
                freeReplyObject(reply);
        } else if (type == "hash") {
            auto *reply = (redisReply *)redisCommand(context, "HGETALL %s", key.c_str());
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                std::stringstream ss;
                ss << "{";
                for (size_t i = 0; i < reply->elements && i < 10; i += 2) {
                    if (i > 0)
                        ss << ", ";
                    ss << "\"" << reply->element[i]->str << "\": \"" << reply->element[i + 1]->str
                       << "\"";
                }
                if (reply->elements > 10)
                    ss << ", ...";
                ss << "}";
                std::string value = ss.str();
                freeReplyObject(reply);
                return value;
            }
            if (reply)
                freeReplyObject(reply);
        } else if (type == "zset") {
            auto *reply =
                (redisReply *)redisCommand(context, "ZRANGE %s 0 4 WITHSCORES", key.c_str());
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                std::stringstream ss;
                ss << "[";
                for (size_t i = 0; i < reply->elements && i < 10; i += 2) {
                    if (i > 0)
                        ss << ", ";
                    ss << "\"" << reply->element[i]->str << "\":" << reply->element[i + 1]->str;
                }
                if (reply->elements > 10)
                    ss << ", ...";
                ss << "]";
                std::string value = ss.str();
                freeReplyObject(reply);
                return value;
            }
            if (reply)
                freeReplyObject(reply);
        }
    } catch (const std::exception &e) {
        std::cerr << "Error getting Redis key value: " << e.what() << std::endl;
    }

    return "[Unable to retrieve value]";
}

std::string RedisDatabase::getKeyType(const std::string &key) const {
    if (!isConnected()) {
        return "unknown";
    }

    try {
        auto *reply = (redisReply *)redisCommand(context, "TYPE %s", key.c_str());
        if (reply && reply->type == REDIS_REPLY_STATUS) {
            std::string type = reply->str;
            freeReplyObject(reply);
            return type;
        }
        if (reply)
            freeReplyObject(reply);
    } catch (const std::exception &e) {
        std::cerr << "Error getting Redis key type: " << e.what() << std::endl;
    }

    return "unknown";
}

int64_t RedisDatabase::getKeyTTL(const std::string &key) const {
    if (!isConnected()) {
        return -1;
    }

    try {
        auto *reply = (redisReply *)redisCommand(context, "TTL %s", key.c_str());
        if (reply && reply->type == REDIS_REPLY_INTEGER) {
            const int64_t ttl = reply->integer;
            freeReplyObject(reply);
            return ttl;
        }
        if (reply)
            freeReplyObject(reply);
    } catch (const std::exception &e) {
        std::cerr << "Error getting Redis key TTL: " << e.what() << std::endl;
    }

    return -1;
}

// Protected methods
std::vector<std::string> RedisDatabase::getTableNames() {
    std::vector<std::string> patterns;

    if (!isConnected()) {
        return patterns;
    }

    // For Redis, we'll return common key patterns
    patterns.emplace_back("*"); // All keys

    return patterns;
}

std::vector<Column> RedisDatabase::getTableColumns(const std::string &keyPattern) {
    std::vector<Column> columns;

    Column keyCol;
    keyCol.name = "Key";
    keyCol.type = "string";
    keyCol.isNotNull = true;
    keyCol.isPrimaryKey = true;
    columns.push_back(keyCol);

    Column typeCol;
    typeCol.name = "Type";
    typeCol.type = "string";
    typeCol.isNotNull = true;
    typeCol.isPrimaryKey = false;
    columns.push_back(typeCol);

    Column valueCol;
    valueCol.name = "Value";
    valueCol.type = "string";
    valueCol.isNotNull = false;
    valueCol.isPrimaryKey = false;
    columns.push_back(valueCol);

    Column ttlCol;
    ttlCol.name = "TTL";
    ttlCol.type = "integer";
    ttlCol.isNotNull = false;
    ttlCol.isPrimaryKey = false;
    columns.push_back(ttlCol);

    return columns;
}

std::vector<std::string> RedisDatabase::getViewNames() {
    return {}; // Redis doesn't have views
}

std::vector<Column> RedisDatabase::getViewColumns(const std::string &viewName) {
    return {}; // Redis doesn't have views
}

std::vector<std::string> RedisDatabase::getSequenceNames() {
    return {}; // Redis doesn't have sequences
}

// Private helper methods
redisReply *RedisDatabase::executeRedisCommand(const std::string &command) const {
    if (!isConnected()) {
        return nullptr;
    }

    try {
        return (redisReply *)redisCommand(context, "%s", command.c_str());
    } catch (const std::exception &e) {
        std::cerr << "Error executing Redis command: " << e.what() << std::endl;
        return nullptr;
    }
}

std::string RedisDatabase::formatRedisReply(redisReply *reply) {
    if (!reply) {
        return "NULL";
    }

    switch (reply->type) {
    case REDIS_REPLY_STRING:
        return {reply->str, reply->len};
    case REDIS_REPLY_ARRAY: {
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < reply->elements; ++i) {
            if (i > 0)
                ss << ", ";
            ss << formatRedisReply(reply->element[i]);
        }
        ss << "]";
        return ss.str();
    }
    case REDIS_REPLY_INTEGER:
        return std::to_string(reply->integer);
    case REDIS_REPLY_NIL:
        return "NULL";
    case REDIS_REPLY_STATUS:
        return {reply->str, reply->len};
    case REDIS_REPLY_ERROR:
        return "ERROR: " + std::string(reply->str, reply->len);
    default:
        return "UNKNOWN";
    }
}

std::vector<std::string> RedisDatabase::parseRedisCommand(const std::string &command) {
    std::vector<std::string> parts;
    std::stringstream ss(command);
    std::string part;

    while (ss >> part) {
        parts.push_back(part);
    }

    return parts;
}

void RedisDatabase::groupKeysByPattern() {
    if (!isConnected()) {
        return;
    }

    // Create a single "table" representing all keys
    // Use "*" as the name so it can be used as a Redis key pattern
    Table allKeys;
    allKeys.name = "*";
    allKeys.columns = getTableColumns("*");
    tables.push_back(allKeys);
}
