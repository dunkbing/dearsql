#ifdef _WIN32
#include <winsock2.h>
#endif

#include "database/redis.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string_view>

namespace {
    struct RedisSSLInit {
        RedisSSLInit() {
            redisInitOpenSSL();
        }
    };
    static RedisSSLInit redisSSLInitOnce;

    using RedisReplyPtr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

    RedisReplyPtr wrapRedisReply(redisReply* reply) {
        return RedisReplyPtr(reply, &freeReplyObject);
    }

    std::string formatRedisPreview(redisReply* reply, std::string_view type) {
        if (!reply) {
            return "[error]";
        }

        if (type == "string") {
            return (reply->type == REDIS_REPLY_STRING) ? std::string(reply->str, reply->len)
                                                       : "[nil]";
        }

        if (type == "list") {
            if (reply->type != REDIS_REPLY_ARRAY) {
                return "[nil]";
            }

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
            return ss.str();
        }

        if (type == "set") {
            const redisReply* members = reply;
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
                reply->element[1]->type == REDIS_REPLY_ARRAY) {
                members = reply->element[1];
            }
            if (members->type != REDIS_REPLY_ARRAY) {
                return "[nil]";
            }

            std::stringstream ss;
            ss << "{";
            for (size_t i = 0; i < members->elements && i < 5; ++i) {
                if (i > 0)
                    ss << ", ";
                ss << "\"" << members->element[i]->str << "\"";
            }
            if (members->elements > 5)
                ss << ", ...";
            ss << "}";
            return ss.str();
        }

        if (type == "hash") {
            const redisReply* pairs = reply;
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
                reply->element[1]->type == REDIS_REPLY_ARRAY) {
                pairs = reply->element[1];
            }
            if (pairs->type != REDIS_REPLY_ARRAY) {
                return "[nil]";
            }

            std::stringstream ss;
            ss << "{";
            for (size_t i = 0; i + 1 < pairs->elements && i < 10; i += 2) {
                if (i > 0)
                    ss << ", ";
                ss << "\"" << pairs->element[i]->str << "\": \"" << pairs->element[i + 1]->str
                   << "\"";
            }
            if (pairs->elements > 10)
                ss << ", ...";
            ss << "}";
            return ss.str();
        }

        if (type == "zset") {
            if (reply->type != REDIS_REPLY_ARRAY) {
                return "[nil]";
            }

            std::stringstream ss;
            ss << "[";
            for (size_t i = 0; i + 1 < reply->elements && i < 10; i += 2) {
                if (i > 0)
                    ss << ", ";
                ss << "\"" << reply->element[i]->str << "\":" << reply->element[i + 1]->str;
            }
            if (reply->elements > 10)
                ss << ", ...";
            ss << "]";
            return ss.str();
        }

        return "";
    }
} // namespace

RedisDatabase::RedisDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
}

RedisDatabase::~RedisDatabase() {
    disconnect();
}

std::pair<bool, std::string> RedisDatabase::connect() {
    std::lock_guard<std::mutex> lock(contextMutex_);

    if (connected && context) {
        return {true, ""};
    }

    if (context) {
        redisFree(context);
        context = nullptr;
    }
    if (sslCtx_) {
        redisFreeSSLContext(sslCtx_);
        sslCtx_ = nullptr;
    }
    connected = false;

    setAttemptedConnection(true);
    auto [prepOk, prepErr] = prepareConnectionForConnect();
    if (!prepOk) {
        setLastConnectionError(prepErr);
        return {false, prepErr};
    }
    std::cout << "Attempting Redis connection to " << connectionInfo.host << ":"
              << connectionInfo.port << std::endl;

    auto cleanupConnectionState = [this]() {
        if (context) {
            redisFree(context);
            context = nullptr;
        }
        if (sslCtx_) {
            redisFreeSSLContext(sslCtx_);
            sslCtx_ = nullptr;
        }
    };

    try {
        constexpr timeval timeout = {5, 0};
        context =
            redisConnectWithTimeout(connectionInfo.host.c_str(), connectionInfo.port, timeout);
        if (!context || context->err) {
            std::string error = context ? context->errstr : "Failed to allocate redis context";
            setLastConnectionError(error);
            std::cout << "Redis connection failed: " << error << std::endl;
            cleanupConnectionState();
            return {false, error};
        }

        if (connectionInfo.sslmode == SslMode::Require ||
            connectionInfo.sslmode == SslMode::VerifyCA ||
            connectionInfo.sslmode == SslMode::VerifyFull) {
            redisSSLContextError sslErr = REDIS_SSL_CTX_NONE;
            const char* caPath = !connectionInfo.sslCACertPath.empty()
                                     ? connectionInfo.sslCACertPath.c_str()
                                     : nullptr;

            sslCtx_ = redisCreateSSLContext(caPath, nullptr, nullptr, nullptr, nullptr, &sslErr);
            if (!sslCtx_) {
                std::string error =
                    std::string("Redis SSL context failed: ") + redisSSLContextGetError(sslErr);
                setLastConnectionError(error);
                cleanupConnectionState();
                return {false, error};
            }

            if (redisInitiateSSLWithContext(context, sslCtx_) != REDIS_OK) {
                std::string error =
                    context->errstr[0] ? context->errstr : "Redis TLS handshake failed";
                setLastConnectionError(error);
                cleanupConnectionState();
                return {false, error};
            }
        }

        if (!connectionInfo.password.empty()) {
            std::cout << "Authenticating with Redis server..." << std::endl;

            redisReply* reply = nullptr;

            if (!connectionInfo.username.empty()) {
                std::cout << "Using Redis ACL authentication with username: "
                          << connectionInfo.username << std::endl;
                reply = (redisReply*)redisCommand(context, "AUTH %s %s",
                                                  connectionInfo.username.c_str(),
                                                  connectionInfo.password.c_str());
            } else {
                std::cout << "Using legacy Redis authentication (password only)" << std::endl;
                reply =
                    (redisReply*)redisCommand(context, "AUTH %s", connectionInfo.password.c_str());
            }

            if (!reply || reply->type == REDIS_REPLY_ERROR) {
                std::string error = reply ? reply->str : "Authentication failed";
                setLastConnectionError(error);
                std::cout << "Redis authentication failed: " << error << std::endl;
                if (reply)
                    freeReplyObject(reply);
                cleanupConnectionState();
                return {false, error};
            }
            freeReplyObject(reply);
            std::cout << "Redis authentication successful" << std::endl;
        }

        auto* reply = (redisReply*)redisCommand(context, "PING");
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            std::string error = reply ? reply->str : "Connection test failed";
            setLastConnectionError(error);
            if (reply)
                freeReplyObject(reply);
            cleanupConnectionState();
            return {false, error};
        }
        freeReplyObject(reply);

        connected = true;
        setLastConnectionError("");
        std::cout << "Successfully connected to Redis: " << connectionInfo.buildConnectionString()
                  << std::endl;
        return {true, ""};
    } catch (const std::exception& e) {
        std::string error = e.what();
        setLastConnectionError(error);
        cleanupConnectionState();
        return {false, error};
    }
}

void RedisDatabase::disconnect() {
    keysLoadOp_.cancel();
    dbInfoLoadOp_.cancel();
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (context) {
            redisFree(context);
            context = nullptr;
            std::cout << "Disconnected from Redis: " << connectionInfo.buildConnectionString()
                      << std::endl;
        }
        if (sslCtx_) {
            redisFreeSSLContext(sslCtx_);
            sslCtx_ = nullptr;
        }
        connected = false;
    }
    stopSshTunnel();

    loadingKeys = false;
    loadingDbInfo_ = false;
    dbInfoLoaded_ = false;
}

void RedisDatabase::refreshConnection() {
    refreshWorkflow_.start([this]() -> bool {
        disconnect();
        setAttemptedConnection(false);
        setLastConnectionError("");

        auto [success, error] = connect();
        if (!success) {
            setLastConnectionError(error);
            return false;
        }

        return true;
    });
}

void RedisDatabase::checkRefreshWorkflowAsync() {
    refreshWorkflow_.check([this](const bool success) {
        if (success) {
            keysLoaded = false;
            dbInfoLoaded_ = false;
            startDbInfoLoadAsync(true);
            startKeysLoadAsync(true);
        }
    });
}

void RedisDatabase::checkTablesStatusAsync() {
    checkKeysStatusAsync();
}

QueryResult RedisDatabase::executeQuery(const std::string& command, int rowLimit) {
    QueryResult result;
    StatementResult s;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!isConnected()) {
        s.success = false;
        s.errorMessage = "Not connected to Redis server";
        result.statements.push_back(std::move(s));
        return result;
    }

    try {
        auto commandParts = parseRedisCommand(command);
        if (commandParts.empty()) {
            s.success = false;
            s.errorMessage = "Empty command";
            result.statements.push_back(std::move(s));
            return result;
        }

        RedisReplyPtr reply = wrapRedisReply(executeRedisCommandParsed(commandParts));
        if (!reply) {
            s.success = false;
            s.errorMessage = "Failed to execute command";
            result.statements.push_back(std::move(s));
            return result;
        }

        if (reply->type == REDIS_REPLY_ERROR) {
            s.success = false;
            s.errorMessage = reply->str;
            result.statements.push_back(std::move(s));
            return result;
        }

        s.columnNames.push_back("result");
        s.tableData.push_back({formatRedisReply(reply.get())});
    } catch (const std::exception& e) {
        s.success = false;
        s.errorMessage = e.what();
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    result.statements.push_back(std::move(s));

    return result;
}

std::vector<std::vector<std::string>> RedisDatabase::getTableData(const std::string& keyPattern,
                                                                  int limit, int offset) {
    std::vector<std::vector<std::string>> data;

    if (!isConnected()) {
        return data;
    }

    try {
        auto keys = getKeys(keyPattern, limit + offset);

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
            row.push_back(std::to_string(it->ttl));

            if (it->size < 0) {
                row.push_back("-");
            } else if (it->size < 1024) {
                row.push_back(std::to_string(it->size) + " B");
            } else if (it->size < 1024 * 1024) {
                row.push_back(std::format("{:.1f} KB", it->size / 1024.0));
            } else {
                row.push_back(std::format("{:.1f} MB", it->size / (1024.0 * 1024.0)));
            }

            data.push_back(row);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> RedisDatabase::getColumnNames(const std::string& keyPattern) {
    return {"Key", "Type", "Value", "TTL", "Size"};
}

int RedisDatabase::getRowCount(const std::string& keyPattern) {
    if (!isConnected()) {
        return 0;
    }

    try {
        const auto keys = getKeys(keyPattern, 10000);
        return static_cast<int>(keys.size());
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key count: " << e.what() << std::endl;
        return 0;
    }
}

std::vector<RedisKey> RedisDatabase::getKeys(const std::string& pattern, const int limit) const {
    std::vector<RedisKey> keys;

    if (!isConnected()) {
        return keys;
    }

    try {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!context) {
            return keys;
        }

        std::vector<std::string> keyNames;
        unsigned long long cursor = 0;
        do {
            auto* reply = static_cast<redisReply*>(
                redisCommand(context, "SCAN %llu MATCH %s COUNT 200", cursor, pattern.c_str()));
            if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
                if (reply)
                    freeReplyObject(reply);
                break;
            }
            cursor = std::strtoull(reply->element[0]->str, nullptr, 10);
            auto* keysArray = reply->element[1];
            for (size_t i = 0; i < keysArray->elements; ++i) {
                if (keysArray->element[i]->type == REDIS_REPLY_STRING) {
                    keyNames.emplace_back(keysArray->element[i]->str);
                    if (static_cast<int>(keyNames.size()) >= limit) {
                        cursor = 0;
                        break;
                    }
                }
            }
            freeReplyObject(reply);
        } while (cursor != 0);

        std::sort(keyNames.begin(), keyNames.end());

        for (const auto& name : keyNames) {
            redisAppendCommand(context, "TYPE %s", name.c_str());
            redisAppendCommand(context, "TTL %s", name.c_str());
            redisAppendCommand(context, "MEMORY USAGE %s", name.c_str());
        }

        keys.resize(keyNames.size());
        for (size_t i = 0; i < keyNames.size(); ++i) {
            keys[i].name = keyNames[i];

            redisReply* typeReply = nullptr;
            redisGetReply(context, reinterpret_cast<void**>(&typeReply));
            if (typeReply && typeReply->type == REDIS_REPLY_STATUS) {
                keys[i].type = typeReply->str;
            } else {
                keys[i].type = "unknown";
            }
            if (typeReply)
                freeReplyObject(typeReply);

            redisReply* ttlReply = nullptr;
            redisGetReply(context, reinterpret_cast<void**>(&ttlReply));
            if (ttlReply && ttlReply->type == REDIS_REPLY_INTEGER) {
                keys[i].ttl = ttlReply->integer;
            }
            if (ttlReply)
                freeReplyObject(ttlReply);

            redisReply* memReply = nullptr;
            redisGetReply(context, reinterpret_cast<void**>(&memReply));
            if (memReply && memReply->type == REDIS_REPLY_INTEGER) {
                keys[i].size = memReply->integer;
            }
            if (memReply)
                freeReplyObject(memReply);
        }

        for (const auto& key : keys) {
            if (key.type == "string") {
                redisAppendCommand(context, "GET %s", key.name.c_str());
            } else if (key.type == "list") {
                redisAppendCommand(context, "LRANGE %s 0 4", key.name.c_str());
            } else if (key.type == "set") {
                redisAppendCommand(context, "SSCAN %s 0 COUNT 5", key.name.c_str());
            } else if (key.type == "zset") {
                redisAppendCommand(context, "ZRANGE %s 0 4 WITHSCORES", key.name.c_str());
            } else if (key.type == "hash") {
                redisAppendCommand(context, "HSCAN %s 0 COUNT 5", key.name.c_str());
            } else {
                redisAppendCommand(context, "TYPE %s", key.name.c_str());
            }
        }

        for (size_t i = 0; i < keys.size(); ++i) {
            redisReply* rawReply = nullptr;
            redisGetReply(context, reinterpret_cast<void**>(&rawReply));
            RedisReplyPtr valReply = wrapRedisReply(rawReply);
            if (!valReply) {
                keys[i].value = "[error]";
                continue;
            }

            keys[i].value = formatRedisPreview(valReply.get(), keys[i].type);
        }
    } catch (const std::exception& e) {
        spdlog::error("Error getting Redis keys: {}", e.what());
    }

    return keys;
}

std::string RedisDatabase::getKeyValue(const std::string& key, const std::string& knownType) const {
    if (!isConnected()) {
        return "";
    }

    try {
        std::string type = knownType.empty() ? getKeyType(key) : knownType;
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!context) {
            return "[Unable to retrieve value]";
        }

        RedisReplyPtr reply(nullptr, &freeReplyObject);
        if (type == "string") {
            reply = wrapRedisReply(
                static_cast<redisReply*>(redisCommand(context, "GET %s", key.c_str())));
        } else if (type == "list") {
            reply = wrapRedisReply(
                static_cast<redisReply*>(redisCommand(context, "LRANGE %s 0 4", key.c_str())));
        } else if (type == "set") {
            reply = wrapRedisReply(
                static_cast<redisReply*>(redisCommand(context, "SMEMBERS %s", key.c_str())));
        } else if (type == "hash") {
            reply = wrapRedisReply(
                static_cast<redisReply*>(redisCommand(context, "HGETALL %s", key.c_str())));
        } else if (type == "zset") {
            reply = wrapRedisReply(static_cast<redisReply*>(
                redisCommand(context, "ZRANGE %s 0 4 WITHSCORES", key.c_str())));
        }

        if (reply) {
            const std::string value = formatRedisPreview(reply.get(), type);
            if (!value.empty()) {
                return value;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key value: " << e.what() << std::endl;
    }

    return "[Unable to retrieve value]";
}

std::string RedisDatabase::getKeyType(const std::string& key) const {
    if (!isConnected()) {
        return "unknown";
    }

    try {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!context) {
            return "unknown";
        }

        RedisReplyPtr reply =
            wrapRedisReply(static_cast<redisReply*>(redisCommand(context, "TYPE %s", key.c_str())));
        if (reply && reply->type == REDIS_REPLY_STATUS) {
            return reply->str;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key type: " << e.what() << std::endl;
    }

    return "unknown";
}

int64_t RedisDatabase::getKeyTTL(const std::string& key) const {
    if (!isConnected()) {
        return -1;
    }

    try {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!context) {
            return -1;
        }

        RedisReplyPtr reply =
            wrapRedisReply(static_cast<redisReply*>(redisCommand(context, "TTL %s", key.c_str())));
        if (reply && reply->type == REDIS_REPLY_INTEGER) {
            return reply->integer;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key TTL: " << e.what() << std::endl;
    }

    return -1;
}

std::vector<std::string> RedisDatabase::getTableNames() {
    std::vector<std::string> patterns;

    if (!isConnected()) {
        return patterns;
    }

    patterns.emplace_back("*");

    return patterns;
}

redisReply*
RedisDatabase::executeRedisCommandParsed(const std::vector<std::string>& commandParts) const {
    if (!isConnected() || commandParts.empty()) {
        std::cerr << "Redis parsed command failed: Not connected or empty command" << std::endl;
        return nullptr;
    }

    try {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!context) {
            std::cerr << "Redis parsed command failed: Context unavailable" << std::endl;
            return nullptr;
        }

        std::vector<const char*> argv;
        std::vector<size_t> argvlen;

        for (const auto& part : commandParts) {
            argv.push_back(part.c_str());
            argvlen.push_back(part.length());
        }

        auto* reply = (redisReply*)redisCommandArgv(context, static_cast<int>(argv.size()),
                                                    argv.data(), argvlen.data());
        if (reply && reply->type == REDIS_REPLY_ERROR) {
            std::cerr << "Redis parsed command error: " << reply->str << std::endl;
        }
        return reply;
    } catch (const std::exception& e) {
        std::cerr << "Error executing parsed Redis command: " << e.what() << std::endl;
        return nullptr;
    }
}

std::string RedisDatabase::formatRedisReply(redisReply* reply) {
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

std::vector<std::string> RedisDatabase::parseRedisCommand(const std::string& command) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;
    char quoteChar = '\0';

    for (size_t i = 0; i < command.length(); ++i) {
        char c = command[i];

        if (!inQuotes) {
            if (c == '"' || c == '\'') {
                inQuotes = true;
                quoteChar = c;
            } else if (std::isspace(c)) {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        } else {
            if (c == quoteChar) {
                inQuotes = false;
                quoteChar = '\0';
            } else {
                current += c;
            }
        }
    }

    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

void RedisDatabase::groupKeysByPattern(std::vector<Table>& out) const {
    if (!isConnected()) {
        return;
    }

    Table allKeys;
    allKeys.name = "*";
    allKeys.fullName = connectionInfo.name + ".*";
    out.push_back(std::move(allKeys));
}

void RedisDatabase::startKeysLoadAsync(bool forceRefresh) {
    if (keysLoadOp_.isRunning()) {
        return;
    }

    if (forceRefresh) {
        tables.clear();
        keysLoaded = false;
        lastKeysError.clear();
    }

    if (!forceRefresh && keysLoaded) {
        return;
    }

    loadingKeys = true;
    keysLoadOp_.start([this]() { return getKeysAsync(); });
}

void RedisDatabase::checkKeysStatusAsync() {
    keysLoadOp_.check([this](std::vector<Table> loadedTables) {
        tables = std::move(loadedTables);
        spdlog::info("Key loading completed. Found {} key groups", tables.size());
        keysLoaded = true;
        loadingKeys = false;
    });
}

std::vector<Table> RedisDatabase::getKeysAsync() {
    std::vector<Table> result;

    try {
        if (!isConnected()) {
            spdlog::error("getKeysAsync: database not connected");
            return result;
        }

        std::lock_guard<std::mutex> opLock(operationMutex_);
        groupKeysByPattern(result);

        spdlog::debug("Finished loading keys. Total key groups: {}", result.size());
    } catch (const std::exception& e) {
        spdlog::error("Error loading keys: {}", e.what());
    }

    return result;
}

bool RedisDatabase::selectDatabase(int dbIndex) {
    if (!isConnected() || dbIndex < 0 || dbIndex >= numDatabases_) {
        return false;
    }

    try {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (!context) {
            return false;
        }

        RedisReplyPtr reply =
            wrapRedisReply(static_cast<redisReply*>(redisCommand(context, "SELECT %d", dbIndex)));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            return false;
        }
        selectedDbIndex_ = dbIndex;
        return true;
    } catch (const std::exception& e) {
        spdlog::error("Error selecting Redis database {}: {}", dbIndex, e.what());
        return false;
    }
}

bool RedisDatabase::beginDatabaseScopedOperation(int dbIndex, int& previousDbIndex) {
    previousDbIndex = selectedDbIndex_;
    return selectDatabase(dbIndex);
}

void RedisDatabase::endDatabaseScopedOperation(int previousDbIndex, int activeDbIndex) {
    if (previousDbIndex != activeDbIndex) {
        selectDatabase(previousDbIndex);
    }
}

std::vector<RedisDbInfo> RedisDatabase::fetchDatabaseInfo() {
    std::vector<RedisDbInfo> result;

    if (!isConnected()) {
        return result;
    }

    try {
        bool configuredDbCountKnown = false;

        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            if (!context) {
                return result;
            }

            auto* configReply =
                static_cast<redisReply*>(redisCommand(context, "CONFIG GET databases"));
            if (configReply && configReply->type == REDIS_REPLY_ARRAY &&
                configReply->elements == 2 && configReply->element[1]->type == REDIS_REPLY_STRING) {
                const int configuredDbCount = std::atoi(configReply->element[1]->str);
                if (configuredDbCount > 0) {
                    numDatabases_ = configuredDbCount;
                    configuredDbCountKnown = true;
                }
            }
            if (configReply)
                freeReplyObject(configReply);
        }

        std::string keyspaceInfo;
        {
            std::lock_guard<std::mutex> lock(contextMutex_);
            if (!context) {
                return result;
            }

            auto* infoReply = static_cast<redisReply*>(redisCommand(context, "INFO keyspace"));
            if (infoReply && infoReply->type == REDIS_REPLY_STRING) {
                keyspaceInfo = std::string(infoReply->str, infoReply->len);
            }
            if (infoReply)
                freeReplyObject(infoReply);
        }

        std::map<int, RedisDbInfo> dbMap;
        std::istringstream stream(keyspaceInfo);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty() || line[0] == '#' || line[0] == '\r')
                continue;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.substr(0, 2) != "db")
                continue;

            auto colonPos = line.find(':');
            if (colonPos == std::string::npos)
                continue;

            int dbIdx = 0;
            try {
                dbIdx = std::stoi(line.substr(2, colonPos - 2));
            } catch (...) {
                continue;
            }

            RedisDbInfo info;
            info.index = dbIdx;
            info.hasKeys = true;

            std::string kvPart = line.substr(colonPos + 1);
            std::istringstream kvStream(kvPart);
            std::string kv;
            while (std::getline(kvStream, kv, ',')) {
                auto eqPos = kv.find('=');
                if (eqPos == std::string::npos)
                    continue;
                std::string key = kv.substr(0, eqPos);
                std::string val = kv.substr(eqPos + 1);
                try {
                    if (key == "keys")
                        info.keys = std::stoll(val);
                    else if (key == "expires")
                        info.expires = std::stoll(val);
                    else if (key == "avg_ttl")
                        info.avgTtl = std::stoll(val);
                } catch (...) {
                }
            }

            dbMap[dbIdx] = info;
        }

        if (!configuredDbCountKnown) {
            int inferredDbCount = selectedDbIndex_ + 1;
            if (!dbMap.empty()) {
                inferredDbCount = std::max(inferredDbCount, dbMap.rbegin()->first + 1);
            }
            numDatabases_ = std::max(1, inferredDbCount);
        }

        result.resize(numDatabases_);
        for (int i = 0; i < numDatabases_; ++i) {
            result[i].index = i;
            auto it = dbMap.find(i);
            if (it != dbMap.end()) {
                result[i] = it->second;
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("Error fetching Redis database info: {}", e.what());
    }

    return result;
}

void RedisDatabase::startDbInfoLoadAsync(bool forceRefresh) {
    if (dbInfoLoadOp_.isRunning()) {
        return;
    }

    if (forceRefresh) {
        dbInfoList_.clear();
        dbInfoLoaded_ = false;
    }

    if (!forceRefresh && dbInfoLoaded_) {
        return;
    }

    loadingDbInfo_ = true;
    dbInfoLoadOp_.start([this]() { return fetchDatabaseInfo(); });
}

void RedisDatabase::checkDbInfoStatusAsync() {
    dbInfoLoadOp_.check([this](std::vector<RedisDbInfo> info) {
        dbInfoList_ = std::move(info);
        dbInfoLoaded_ = true;
        loadingDbInfo_ = false;
        spdlog::info("Redis database info loaded. Found {} databases", dbInfoList_.size());
    });
}

std::pair<std::vector<std::string>, std::vector<std::vector<std::string>>>
RedisDatabase::getTableDataForDatabase(int dbIndex, const std::string& pattern, int limit,
                                       int offset) {
    std::lock_guard<std::mutex> opLock(operationMutex_);
    int previousDbIndex = 0;
    if (!beginDatabaseScopedOperation(dbIndex, previousDbIndex)) {
        return {};
    }
    auto cols = getColumnNames(pattern);
    auto data = getTableData(pattern, limit, offset);
    endDatabaseScopedOperation(previousDbIndex, dbIndex);
    return {std::move(cols), std::move(data)};
}

QueryResult RedisDatabase::executeQueryInDatabase(int dbIndex, const std::string& query,
                                                  int rowLimit) {
    std::lock_guard<std::mutex> opLock(operationMutex_);
    int previousDbIndex = 0;
    if (!beginDatabaseScopedOperation(dbIndex, previousDbIndex)) {
        QueryResult result;
        StatementResult s;
        s.success = false;
        s.errorMessage = std::format("Failed to switch to Redis database db{}", dbIndex);
        result.statements.push_back(std::move(s));
        return result;
    }

    QueryResult result = executeQuery(query, rowLimit);
    endDatabaseScopedOperation(previousDbIndex, dbIndex);
    return result;
}
