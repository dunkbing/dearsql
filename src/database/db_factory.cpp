#include "database/cassandra.hpp"
#include "database/db_interface.hpp"
#include "database/file_database.hpp"
#include "database/mongodb.hpp"
#include "database/mssql.hpp"
#include "database/mysql.hpp"
#include "database/oracle.hpp"
#include "database/postgresql.hpp"
#include "database/redis.hpp"
#include "database/s3.hpp"
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
    case DatabaseType::TIDB:
        return make<MySQLDatabase>(info);

    case DatabaseType::MSSQL:
        return make<MSSQLDatabase>(info);

    case DatabaseType::ORACLE:
        return make<OracleDatabase>(info);

    case DatabaseType::REDSHIFT:
    case DatabaseType::COCKROACHDB:
        return make<PostgresDatabase>(info);

    case DatabaseType::CASSANDRA:
    case DatabaseType::SCYLLADB:
        return make<CassandraDatabase>(info);

    case DatabaseType::DUCKDB:
        return make<FileDatabase>(info);

    case DatabaseType::S3:
        return make<S3Database>(info);

    default:
        return nullptr;
    }
}
