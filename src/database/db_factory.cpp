#include "database/cassandra.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"

std::shared_ptr<DatabaseInterface>
DatabaseFactory::createDatabase(const DatabaseConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return std::make_shared<FileDatabase>(info);

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

    case DatabaseType::MSSQL:
        return std::make_shared<MSSQLDatabase>(info);

    case DatabaseType::ORACLE:
        return std::make_shared<OracleDatabase>(info);

    case DatabaseType::REDSHIFT:
        return std::make_shared<PostgresDatabase>(info);

    case DatabaseType::CASSANDRA:
        return std::make_shared<CassandraDatabase>(info);

    case DatabaseType::DUCKDB:
        return std::make_shared<FileDatabase>(info);

    default:
        return nullptr;
    }
}
