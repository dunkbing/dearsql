#include "database/redis.hpp"
#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <sstream>

namespace {
    struct RedisSSLInit {
        RedisSSLInit() {
            redisInitOpenSSL();
        }
    };
    static RedisSSLInit redisSSLInitOnce;
} // namespace

RedisDatabase::RedisDatabase(const DatabaseConnectionInfo& connInfo) {
    this->connectionInfo = connInfo;
}

RedisDatabase::~RedisDatabase() {
    disconnect();
}

std::pair<bool, std::string> RedisDatabase::connect() {
    if (connected && context) {
        return {true, ""};
    }

    // Reset connection state
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
        // Use redisConnectWithTimeout for better timeout handling
        constexpr timeval timeout = {5, 0}; // 5 seconds timeout
        context =
            redisConnectWithTimeout(connectionInfo.host.c_str(), connectionInfo.port, timeout);
        if (!context || context->err) {
            std::string error = context ? context->errstr : "Failed to allocate redis context";
            setLastConnectionError(error);
            std::cout << "Redis connection failed: " << error << std::endl;
            cleanupConnectionState();
            return {false, error};
        }

        // TLS negotiation
        if (connectionInfo.sslmode == SslMode::Require ||
            connectionInfo.sslmode == SslMode::VerifyCA ||
            connectionInfo.sslmode == SslMode::VerifyFull) {
            redisSSLContextError sslErr = REDIS_SSL_CTX_NONE;
            const char* caPath = (!connectionInfo.sslCACertPath.empty() &&
                                  (connectionInfo.sslmode == SslMode::VerifyCA ||
                                   connectionInfo.sslmode == SslMode::VerifyFull))
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

        // Authenticate if password is provided
        if (!connectionInfo.password.empty()) {
            std::cout << "Authenticating with Redis server..." << std::endl;

            redisReply* reply = nullptr;

            // Use Redis 6+ ACL authentication if username is provided
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

        // Test connection with PING
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
    stopSshTunnel();
    connected = false;

    // Reset loading states
    loadingKeys = false;
}

void RedisDatabase::refreshConnection() {
    // Disconnect and reset state
    disconnect();
    setAttemptedConnection(false);
    setLastConnectionError("");

    // Reconnect with force refresh
    auto [success, error] = connect();
    if (!success) {
        setLastConnectionError(error);
        return;
    }

    // Refresh keys after reconnection
    startKeysLoadAsync(true);
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
        // Parse command into parts
        auto commandParts = parseRedisCommand(command);
        if (commandParts.empty()) {
            s.success = false;
            s.errorMessage = "Empty command";
            result.statements.push_back(std::move(s));
            return result;
        }

        redisReply* reply = executeRedisCommandParsed(commandParts);
        if (!reply) {
            s.success = false;
            s.errorMessage = "Failed to execute command";
            result.statements.push_back(std::move(s));
            return result;
        }

        // Check for Redis errors
        if (reply->type == REDIS_REPLY_ERROR) {
            s.success = false;
            s.errorMessage = reply->str;
            freeReplyObject(reply);
            result.statements.push_back(std::move(s));
            return result;
        }

        // Format result as a single-column table for consistency
        s.columnNames.push_back("result");
        std::string formattedReply = formatRedisReply(reply);
        s.tableData.push_back({formattedReply});
        s.message = "Command executed successfully";
        s.success = true;

        freeReplyObject(reply);
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
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key data: " << e.what() << std::endl;
    }

    return data;
}

std::vector<std::string> RedisDatabase::getColumnNames(const std::string& keyPattern) {
    return {"Key", "Type", "Value", "TTL"};
}

int RedisDatabase::getRowCount(const std::string& keyPattern) {
    if (!isConnected()) {
        return 0;
    }

    try {
        const auto keys = getKeys(keyPattern, 10000); // Get up to 10k keys for count
        return static_cast<int>(keys.size());
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis key count: " << e.what() << std::endl;
        return 0;
    }
}

// Redis-specific methods
std::vector<RedisKey> RedisDatabase::getKeys(const std::string& pattern, const int limit) const {
    std::vector<RedisKey> keys;

    if (!isConnected()) {
        return keys;
    }

    try {
        auto* reply = static_cast<redisReply*>(redisCommand(context, "KEYS %s", pattern.c_str()));
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
    } catch (const std::exception& e) {
        std::cerr << "Error getting Redis keys: " << e.what() << std::endl;
    }

    return keys;
}

std::string RedisDatabase::getKeyValue(const std::string& key) const {
    if (!isConnected()) {
        return "";
    }

    try {
        std::string type = getKeyType(key);

        if (type == "string") {
            auto* reply = static_cast<redisReply*>(redisCommand(context, "GET %s", key.c_str()));
            if (reply && reply->type == REDIS_REPLY_STRING) {
                std::string value = reply->str;
                freeReplyObject(reply);
                return value;
            }
            if (reply)
                freeReplyObject(reply);
        } else if (type == "list") {
            auto* reply = (redisReply*)redisCommand(context, "LRANGE %s 0 4", key.c_str());
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
            auto* reply = (redisReply*)redisCommand(context, "SMEMBERS %s", key.c_str());
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
            auto* reply =
                static_cast<redisReply*>(redisCommand(context, "HGETALL %s", key.c_str()));
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
            auto* reply =
                (redisReply*)redisCommand(context, "ZRANGE %s 0 4 WITHSCORES", key.c_str());
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
        auto* reply = (redisReply*)redisCommand(context, "TYPE %s", key.c_str());
        if (reply && reply->type == REDIS_REPLY_STATUS) {
            std::string type = reply->str;
            freeReplyObject(reply);
            return type;
        }
        if (reply)
            freeReplyObject(reply);
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
        auto* reply = (redisReply*)redisCommand(context, "TTL %s", key.c_str());
        if (reply && reply->type == REDIS_REPLY_INTEGER) {
            const int64_t ttl = reply->integer;
            freeReplyObject(reply);
            return ttl;
        }
        if (reply)
            freeReplyObject(reply);
    } catch (const std::exception& e) {
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

// Private helper methods
redisReply* RedisDatabase::executeRedisCommand(const std::string& command) const {
    if (!isConnected()) {
        std::cerr << "Redis command failed: Not connected" << std::endl;
        return nullptr;
    }

    try {
        auto* reply = (redisReply*)redisCommand(context, "%s", command.c_str());
        if (reply && reply->type == REDIS_REPLY_ERROR) {
            std::cerr << "Redis command error: " << reply->str << std::endl;
        }
        return reply;
    } catch (const std::exception& e) {
        std::cerr << "Error executing Redis command: " << e.what() << std::endl;
        return nullptr;
    }
}

redisReply*
RedisDatabase::executeRedisCommandParsed(const std::vector<std::string>& commandParts) const {
    if (!isConnected() || commandParts.empty()) {
        std::cerr << "Redis parsed command failed: Not connected or empty command" << std::endl;
        return nullptr;
    }

    try {
        // Convert string vector to char* array for redisCommandArgv
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

void RedisDatabase::groupKeysByPattern() {
    if (!isConnected()) {
        return;
    }

    // Create a single "table" representing all keys
    // Use "*" as the name so it can be used as a Redis key pattern
    Table allKeys;
    allKeys.name = "*";
    allKeys.fullName = connectionInfo.name + ".*";
    // allKeys.columns = getTableColumns("*");
    tables.push_back(allKeys);
}

// Async key loading methods (merged from RedisNode)
void RedisDatabase::startKeysLoadAsync(bool forceRefresh) {
    if (loadingKeys.load()) {
        return; // already loading
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
    keysFuture = std::async(std::launch::async, [this]() { return getKeysAsync(); });
}

void RedisDatabase::checkKeysStatusAsync() {
    if (keysFuture.valid() &&
        keysFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            tables = keysFuture.get();
            std::cout << std::format("Key loading completed. Found {} key groups", tables.size())
                      << std::endl;
            keysLoaded = true;
            loadingKeys = false;
        } catch (const std::exception& e) {
            std::cerr << std::format("Error in key loading: {}", e.what()) << std::endl;
            lastKeysError = e.what();
            keysLoaded = true;
            loadingKeys = false;
        }
    }
}

std::vector<Table> RedisDatabase::getKeysAsync() {
    std::vector<Table> result;

    try {
        if (!isConnected()) {
            std::cerr << "Database not connected" << std::endl;
            return result;
        }

        // Group keys by pattern
        groupKeysByPattern();
        result = tables;

        std::cout << "Finished loading keys. Total key groups: " << std::to_string(result.size())
                  << std::endl;
    } catch (const std::exception& e) {
        std::cerr << std::format("Error loading keys: {}", e.what()) << std::endl;
    }

    return result;
}
