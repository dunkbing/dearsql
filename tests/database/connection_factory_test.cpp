#include "database/connection_factory.hpp"

#include <gtest/gtest.h>

class ConnectionFactoryTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(ConnectionFactoryTest, BuildsSQLiteConnectionString) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.path = "/path/to/database.db";

    std::string connStr = info.buildConnectionString();
    EXPECT_EQ(connStr, "/path/to/database.db");
}

TEST_F(ConnectionFactoryTest, BuildsPostgreSQLConnectionString) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.host = "localhost";
    info.port = 5432;
    info.database = "testdb";
    info.username = "user";
    info.password = "pass";

    std::string connStr = info.buildConnectionString();
    EXPECT_TRUE(connStr.find("host=localhost") != std::string::npos);
    EXPECT_TRUE(connStr.find("port=5432") != std::string::npos);
    EXPECT_TRUE(connStr.find("dbname=testdb") != std::string::npos);
    EXPECT_TRUE(connStr.find("user=user") != std::string::npos);
    EXPECT_TRUE(connStr.find("password=pass") != std::string::npos);
}

TEST_F(ConnectionFactoryTest, BuildsMySQLConnectionString) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::MYSQL;
    info.host = "localhost";
    info.port = 3306;
    info.database = "testdb";
    info.username = "user";
    info.password = "pass";

    std::string connStr = info.buildConnectionString();
    EXPECT_TRUE(connStr.find("host=localhost") != std::string::npos);
    EXPECT_TRUE(connStr.find("port=3306") != std::string::npos);
    EXPECT_TRUE(connStr.find("dbname=testdb") != std::string::npos);
    EXPECT_TRUE(connStr.find("user=user") != std::string::npos);
    EXPECT_TRUE(connStr.find("password=pass") != std::string::npos);
}

TEST_F(ConnectionFactoryTest, BuildsRedisConnectionString) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::REDIS;
    info.host = "localhost";
    info.port = 6379;

    std::string connStr = info.buildConnectionString();
    EXPECT_EQ(connStr, "redis://localhost:6379");
}

TEST_F(ConnectionFactoryTest, BuildsMongoDBConnectionString) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::MONGODB;
    info.host = "localhost";
    info.port = 27017;
    info.database = "testdb";

    std::string connStr = info.buildConnectionString();
    EXPECT_TRUE(connStr.find("mongodb://") != std::string::npos);
    EXPECT_TRUE(connStr.find("localhost:27017") != std::string::npos);
    EXPECT_TRUE(connStr.find("/testdb") != std::string::npos);
}

TEST_F(ConnectionFactoryTest, BuildsMongoDBConnectionStringWithAuth) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::MONGODB;
    info.host = "localhost";
    info.port = 27017;
    info.database = "testdb";
    info.username = "user";
    info.password = "pass";

    std::string connStr = info.buildConnectionString();
    EXPECT_TRUE(connStr.find("mongodb://user:pass@") != std::string::npos);
}

// ========== Factory Create Tests ==========

TEST_F(ConnectionFactoryTest, CreatesSQLiteDatabase) {
    DatabaseConnectionInfo info;
    info.name = "SQLiteTest";
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
    DatabaseConnectionInfo info;
    info.name = "SQLiteTest";
    info.type = DatabaseType::SQLITE;
    info.path = ":memory:";

    auto [success, error] = ConnectionFactory::testConnection(info);
    EXPECT_TRUE(success) << error;
}

TEST_F(ConnectionFactoryTest, TestConnectionFailsForInvalidSQLitePath) {
    DatabaseConnectionInfo info;
    info.name = "InvalidSQLite";
    info.type = DatabaseType::SQLITE;
    info.path = "/nonexistent/directory/that/does/not/exist/database.db";

    auto [success, error] = ConnectionFactory::testConnection(info);
    EXPECT_FALSE(success);
    EXPECT_FALSE(error.empty());
}

// ========== Default Value Tests ==========

TEST_F(ConnectionFactoryTest, DefaultDatabaseConnectionInfo) {
    DatabaseConnectionInfo info;

    EXPECT_EQ(info.name, "");
    EXPECT_EQ(info.host, "");
    EXPECT_EQ(info.port, 5432); // Default port
    EXPECT_EQ(info.database, "");
    EXPECT_EQ(info.username, "");
    EXPECT_EQ(info.password, "");
    EXPECT_EQ(info.path, "");
    EXPECT_EQ(info.showAllDatabases, false);
}
