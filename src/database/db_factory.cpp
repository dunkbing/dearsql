#include "database/db_interface.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"

// Helper functions to convert between DatabaseType enum and strings
std::string databaseTypeToString(const DatabaseType type) {
    switch (type) {
    case DatabaseType::SQLITE:
        return "sqlite";
    case DatabaseType::POSTGRESQL:
        return "postgresql";
    case DatabaseType::MYSQL:
        return "mysql";
    case DatabaseType::REDIS:
        return "redis";
    case DatabaseType::MONGODB:
        return "mongodb";
    case DatabaseType::MARIADB:
        return "mariadb";
    }
    return "unknown";
}

DatabaseType stringToDatabaseType(const std::string& typeStr) {
    if (typeStr == "sqlite")
        return DatabaseType::SQLITE;
    if (typeStr == "postgresql")
        return DatabaseType::POSTGRESQL;
    if (typeStr == "mysql")
        return DatabaseType::MYSQL;
    if (typeStr == "redis")
        return DatabaseType::REDIS;
    if (typeStr == "mongodb")
        return DatabaseType::MONGODB;
    if (typeStr == "mariadb")
        return DatabaseType::MARIADB;
    return DatabaseType::SQLITE; // default
}

std::string sslModeToString(const SslMode mode) {
    switch (mode) {
    case SslMode::Disable:
        return "disable";
    case SslMode::Allow:
        return "allow";
    case SslMode::Prefer:
        return "prefer";
    case SslMode::Require:
        return "require";
    case SslMode::VerifyCA:
        return "verify-ca";
    case SslMode::VerifyFull:
        return "verify-full";
    }
    return "prefer";
}

SslMode stringToSslMode(const std::string& str) {
    if (str == "disable")
        return SslMode::Disable;
    if (str == "allow")
        return SslMode::Allow;
    if (str == "prefer")
        return SslMode::Prefer;
    if (str == "require")
        return SslMode::Require;
    if (str == "verify-ca")
        return SslMode::VerifyCA;
    if (str == "verify-full")
        return SslMode::VerifyFull;
    return SslMode::Prefer;
}

std::shared_ptr<DatabaseInterface>
DatabaseFactory::createDatabase(const DatabaseConnectionInfo& info) {
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

    case DatabaseType::MARIADB:
        return std::make_shared<MySQLDatabase>(info);

    default:
        return nullptr;
    }
}
