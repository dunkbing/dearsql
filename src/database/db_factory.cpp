#include "database/db_interface.hpp"
#include "database/mysql.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/sqlite.hpp"

std::shared_ptr<DatabaseInterface>
DatabaseFactory::createDatabase(const DatabaseConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return std::make_shared<SQLiteDatabase>(info.name, info.path);

    case DatabaseType::POSTGRESQL:
        return std::make_shared<PostgresDatabase>(info.name, info.host, info.port, info.database,
                                                  info.username, info.password);

    case DatabaseType::MYSQL:
        return std::make_shared<MySQLDatabase>(info.name, info.host, info.port, info.database,
                                               info.username, info.password);

    case DatabaseType::REDIS:
        return std::make_shared<RedisDatabase>(info.name, info.host, info.port, info.password);

    default:
        return nullptr;
    }
}
