#include "database/cassandra.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "utils/reaper.hpp"

namespace {
    // whoever lets go last, the destructor (it joins connect/list/load workers and
    // closes the connection) runs on the reaper, never on the UI thread
    template <typename T>
    std::shared_ptr<DatabaseInterface> make(const DatabaseConnectionInfo& info) {
        return std::shared_ptr<T>(new T(info), [](T* db) { Reaper::post([db] { delete db; }); });
    }
} // namespace

std::shared_ptr<DatabaseInterface>
DatabaseFactory::createDatabase(const DatabaseConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return make<FileDatabase>(info);

    case DatabaseType::POSTGRESQL:
        return make<PostgresDatabase>(info);

    case DatabaseType::MYSQL:
        return make<MySQLDatabase>(info);

    case DatabaseType::REDIS:
        return make<RedisDatabase>(info);

    case DatabaseType::MONGODB:
        return make<MongoDBDatabase>(info);

    case DatabaseType::MARIADB:
        return make<MySQLDatabase>(info);

    case DatabaseType::MSSQL:
        return make<MSSQLDatabase>(info);

    case DatabaseType::ORACLE:
        return make<OracleDatabase>(info);

    case DatabaseType::REDSHIFT:
        return make<PostgresDatabase>(info);

    case DatabaseType::CASSANDRA:
        return make<CassandraDatabase>(info);

    case DatabaseType::DUCKDB:
        return make<FileDatabase>(info);

    default:
        return nullptr;
    }
}
