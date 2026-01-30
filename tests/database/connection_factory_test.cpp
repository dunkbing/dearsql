#include "database/connection_factory.hpp"

#include <gtest/gtest.h>

class ConnectionFactoryTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(ConnectionFactoryTest, ConnectionConfigBuildsSQLiteString) {
    ConnectionConfig config;
    config.type = DatabaseType::SQLITE;
    config.filePath = "/path/to/database.db";

    std::string connStr = config.buildConnectionString();
    EXPECT_EQ(connStr, "/path/to/database.db");
}

TEST_F(ConnectionFactoryTest, ConnectionConfigBuildsPostgreSQLString) {
    ConnectionConfig config;
    config.type = DatabaseType::POSTGRESQL;
    config.host = "localhost";
    config.port = 5432;
    config.database = "testdb";
    config.username = "user";
    config.password = "pass";

    std::string connStr = config.buildConnectionString();
    EXPECT_TRUE(connStr.find("host=localhost") != std::string::npos);
    EXPECT_TRUE(connStr.find("port=5432") != std::string::npos);
    EXPECT_TRUE(connStr.find("dbname=testdb") != std::string::npos);
    EXPECT_TRUE(connStr.find("user=user") != std::string::npos);
    EXPECT_TRUE(connStr.find("password=pass") != std::string::npos);
}

TEST_F(ConnectionFactoryTest, ConnectionConfigBuildsMySQLString) {
    ConnectionConfig config;
    config.type = DatabaseType::MYSQL;
    config.host = "localhost";
    config.port = 3306;
    config.database = "testdb";
    config.username = "user";
    config.password = "pass";

    std::string connStr = config.buildConnectionString();
    EXPECT_TRUE(connStr.find("host=localhost") != std::string::npos);
    EXPECT_TRUE(connStr.find("port=3306") != std::string::npos);
    EXPECT_TRUE(connStr.find("db=testdb") != std::string::npos);
    EXPECT_TRUE(connStr.find("user=user") != std::string::npos);
    EXPECT_TRUE(connStr.find("password=pass") != std::string::npos);
}

TEST_F(ConnectionFactoryTest, ConnectionConfigBuildsRedisString) {
    ConnectionConfig config;
    config.type = DatabaseType::REDIS;
    config.host = "localhost";
    config.port = 6379;

    std::string connStr = config.buildConnectionString();
    EXPECT_EQ(connStr, "localhost:6379");
}

TEST_F(ConnectionFactoryTest, ConnectionConfigRedisDefaultValues) {
    ConnectionConfig config;
    config.type = DatabaseType::REDIS;
    // host empty, port 0

    std::string connStr = config.buildConnectionString();
    EXPECT_EQ(connStr, "127.0.0.1:6379");
}

TEST_F(ConnectionFactoryTest, ConnectionConfigPartialPostgreSQL) {
    ConnectionConfig config;
    config.type = DatabaseType::POSTGRESQL;
    config.host = "localhost";
    config.database = "testdb";
    // No port, username, or password

    std::string connStr = config.buildConnectionString();
    EXPECT_TRUE(connStr.find("host=localhost") != std::string::npos);
    EXPECT_TRUE(connStr.find("dbname=testdb") != std::string::npos);
    EXPECT_TRUE(connStr.find("port=") == std::string::npos);
    EXPECT_TRUE(connStr.find("user=") == std::string::npos);
    EXPECT_TRUE(connStr.find("password=") == std::string::npos);
}

// ========== Conversion Tests ==========

TEST_F(ConnectionFactoryTest, ConnectionConfigToDatabaseConnectionInfo) {
    ConnectionConfig config;
    config.name = "TestConnection";
    config.type = DatabaseType::POSTGRESQL;
    config.host = "localhost";
    config.port = 5432;
    config.database = "testdb";
    config.username = "user";
    config.password = "pass";
    config.filePath = "/some/path";
    config.showAllDatabases = true;

    DatabaseConnectionInfo info = config.toDatabaseConnectionInfo();

    EXPECT_EQ(info.name, "TestConnection");
    EXPECT_EQ(info.type, DatabaseType::POSTGRESQL);
    EXPECT_EQ(info.host, "localhost");
    EXPECT_EQ(info.port, 5432);
    EXPECT_EQ(info.database, "testdb");
    EXPECT_EQ(info.username, "user");
    EXPECT_EQ(info.password, "pass");
    EXPECT_EQ(info.path, "/some/path");
    EXPECT_EQ(info.showAllDatabases, true);
}

TEST_F(ConnectionFactoryTest, ConnectionConfigFromDatabaseConnectionInfo) {
    DatabaseConnectionInfo info;
    info.name = "FromInfo";
    info.type = DatabaseType::MYSQL;
    info.host = "dbserver";
    info.port = 3306;
    info.database = "mydb";
    info.username = "admin";
    info.password = "secret";
    info.path = "/data/db";
    info.showAllDatabases = false;

    ConnectionConfig config = ConnectionConfig::fromDatabaseConnectionInfo(info);

    EXPECT_EQ(config.name, "FromInfo");
    EXPECT_EQ(config.type, DatabaseType::MYSQL);
    EXPECT_EQ(config.host, "dbserver");
    EXPECT_EQ(config.port, 3306);
    EXPECT_EQ(config.database, "mydb");
    EXPECT_EQ(config.username, "admin");
    EXPECT_EQ(config.password, "secret");
    EXPECT_EQ(config.filePath, "/data/db");
    EXPECT_EQ(config.showAllDatabases, false);
}

TEST_F(ConnectionFactoryTest, RoundTripConversion) {
    ConnectionConfig original;
    original.name = "RoundTrip";
    original.type = DatabaseType::SQLITE;
    original.host = "localhost";
    original.port = 1234;
    original.database = "testdb";
    original.username = "user";
    original.password = "pass";
    original.filePath = "/path/to/db";
    original.showAllDatabases = true;

    DatabaseConnectionInfo info = original.toDatabaseConnectionInfo();
    ConnectionConfig restored = ConnectionConfig::fromDatabaseConnectionInfo(info);

    EXPECT_EQ(restored.name, original.name);
    EXPECT_EQ(restored.type, original.type);
    EXPECT_EQ(restored.host, original.host);
    EXPECT_EQ(restored.port, original.port);
    EXPECT_EQ(restored.database, original.database);
    EXPECT_EQ(restored.username, original.username);
    EXPECT_EQ(restored.password, original.password);
    EXPECT_EQ(restored.filePath, original.filePath);
    EXPECT_EQ(restored.showAllDatabases, original.showAllDatabases);
}

// ========== Factory Create Tests ==========

TEST_F(ConnectionFactoryTest, CreatesSQLiteDatabase) {
    ConnectionConfig config;
    config.name = "SQLiteTest";
    config.type = DatabaseType::SQLITE;
    config.filePath = ":memory:";

    auto db = ConnectionFactory::create(config);
    ASSERT_NE(db, nullptr);
    // Verify connection works
    auto [success, error] = db->connect();
    EXPECT_TRUE(success) << error;
    db->disconnect();
}

TEST_F(ConnectionFactoryTest, CreatesFromDatabaseConnectionInfo) {
    DatabaseConnectionInfo info;
    info.name = "SQLiteInfo";
    info.type = DatabaseType::SQLITE;
    info.path = ":memory:";

    auto db = ConnectionFactory::create(info);
    ASSERT_NE(db, nullptr);
    // Verify connection works
    auto [success, error] = db->connect();
    EXPECT_TRUE(success) << error;
    db->disconnect();
}

TEST_F(ConnectionFactoryTest, TestConnectionSucceedsForSQLite) {
    ConnectionConfig config;
    config.name = "SQLiteTest";
    config.type = DatabaseType::SQLITE;
    config.filePath = ":memory:";

    auto [success, error] = ConnectionFactory::testConnection(config);
    EXPECT_TRUE(success) << error;
}

TEST_F(ConnectionFactoryTest, TestConnectionFailsForInvalidSQLitePath) {
    // Test with an invalid SQLite path (directory that doesn't exist)
    ConnectionConfig config;
    config.name = "InvalidSQLite";
    config.type = DatabaseType::SQLITE;
    config.filePath = "/nonexistent/directory/that/does/not/exist/database.db";

    auto [success, error] = ConnectionFactory::testConnection(config);
    EXPECT_FALSE(success);
    EXPECT_FALSE(error.empty());
}

// ========== Default Value Tests ==========

TEST_F(ConnectionFactoryTest, DefaultConnectionConfig) {
    ConnectionConfig config;

    EXPECT_EQ(config.name, "");
    EXPECT_EQ(config.type, DatabaseType::SQLITE);
    EXPECT_EQ(config.host, "");
    EXPECT_EQ(config.port, 0);
    EXPECT_EQ(config.database, "");
    EXPECT_EQ(config.username, "");
    EXPECT_EQ(config.password, "");
    EXPECT_EQ(config.filePath, "");
    EXPECT_EQ(config.showAllDatabases, false);
    EXPECT_EQ(config.redisDb, 0);
    EXPECT_EQ(config.poolSize, 5);
}
