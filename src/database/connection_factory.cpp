#include "database/connection_factory.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"

std::string ConnectionConfig::buildConnectionString() const {
    switch (type) {
    case DatabaseType::SQLITE:
        return filePath;

    case DatabaseType::POSTGRESQL: {
        std::string connStr;
        if (!host.empty()) {
            connStr += "host=" + host + " ";
        }
        if (port > 0) {
            connStr += "port=" + std::to_string(port) + " ";
        }
        if (!database.empty()) {
            connStr += "dbname=" + database + " ";
        }
        if (!username.empty()) {
            connStr += "user=" + username + " ";
        }
        if (!password.empty()) {
            connStr += "password=" + password + " ";
        }
        return connStr;
    }

    case DatabaseType::MYSQL: {
        std::string connStr;
        if (!host.empty()) {
            connStr += "host=" + host + " ";
        }
        if (port > 0) {
            connStr += "port=" + std::to_string(port) + " ";
        }
        if (!database.empty()) {
            connStr += "db=" + database + " ";
        }
        if (!username.empty()) {
            connStr += "user=" + username + " ";
        }
        if (!password.empty()) {
            connStr += "password=" + password + " ";
        }
        return connStr;
    }

    case DatabaseType::REDIS: {
        std::string connStr = host.empty() ? "127.0.0.1" : host;
        connStr += ":" + std::to_string(port > 0 ? port : 6379);
        return connStr;
    }

    case DatabaseType::MONGODB: {
        std::string connStr = "mongodb://";
        if (!username.empty()) {
            connStr += username;
            if (!password.empty()) {
                connStr += ":" + password;
            }
            connStr += "@";
        }
        connStr += (host.empty() ? "127.0.0.1" : host);
        connStr += ":" + std::to_string(port > 0 ? port : 27017);
        if (!database.empty()) {
            connStr += "/" + database;
        }
        return connStr;
    }

    default:
        return "";
    }
}

DatabaseConnectionInfo ConnectionConfig::toDatabaseConnectionInfo() const {
    DatabaseConnectionInfo info;
    info.name = name;
    info.type = type;
    info.host = host;
    info.port = port;
    info.database = database;
    info.username = username;
    info.password = password;
    info.path = filePath;
    info.showAllDatabases = showAllDatabases;
    return info;
}

ConnectionConfig ConnectionConfig::fromDatabaseConnectionInfo(const DatabaseConnectionInfo& info) {
    ConnectionConfig config;
    config.name = info.name;
    config.type = info.type;
    config.host = info.host;
    config.port = info.port;
    config.database = info.database;
    config.username = info.username;
    config.password = info.password;
    config.filePath = info.path;
    config.showAllDatabases = info.showAllDatabases;
    return config;
}

// ========== ConnectionFactory Implementation ==========

std::shared_ptr<DatabaseInterface> ConnectionFactory::create(const ConnectionConfig& config) {
    return create(config.toDatabaseConnectionInfo());
}

std::shared_ptr<DatabaseInterface> ConnectionFactory::create(const DatabaseConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return std::make_shared<SQLiteDatabase>(info);

    case DatabaseType::POSTGRESQL:
        return std::make_shared<PostgresDatabase>(info);

    case DatabaseType::MYSQL:
        return std::make_shared<MySQLDatabase>(info);

    case DatabaseType::REDIS:
        return std::make_shared<RedisDatabase>(info);

    case DatabaseType::MONGODB:
        return std::make_shared<MongoDBDatabase>(info);

    default:
        return nullptr;
    }
}

std::pair<bool, std::string> ConnectionFactory::testConnection(const ConnectionConfig& config) {
    auto db = create(config);
    if (!db) {
        return {false, "Failed to create database interface"};
    }

    auto [success, error] = db->connect();
    if (success) {
        db->disconnect();
    }
    return {success, error};
}
