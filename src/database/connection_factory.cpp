#include "database/connection_factory.hpp"
#include "database/mongodb.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"

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

std::pair<bool, std::string> ConnectionFactory::testConnection(const DatabaseConnectionInfo& info) {
    auto db = create(info);
    if (!db) {
        return {false, "Failed to create database interface"};
    }

    auto [success, error] = db->connect();
    if (success) {
        db->disconnect();
    }
    return {success, error};
}
