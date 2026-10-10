#include "database/cassandra.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct CassandraConfig {
        std::string name = "CassandraIntegration";
        std::string host;
        int port = 9042;
        std::string user;
        std::string password;
    };

    std::optional<CassandraConfig> loadCassandraConfigFromEnv() {
        CassandraConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_CASSANDRA_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_CASSANDRA_PORT");
        const char* userEnv = std::getenv("DEARSQL_TEST_CASSANDRA_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_CASSANDRA_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_CASSANDRA_NAME");

        if (!hostEnv) {
            return std::nullopt;
        }

        cfg.host = hostEnv;
        if (userEnv) {
            cfg.user = userEnv;
        }
        if (passwordEnv) {
            cfg.password = passwordEnv;
        }
        if (nameEnv && *nameEnv != '\0') {
            cfg.name = nameEnv;
        }
        if (portEnv && *portEnv != '\0') {
            cfg.port = std::stoi(portEnv);
        }
        return cfg;
    }
} // namespace

class CassandraDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadCassandraConfigFromEnv();
        if (!cfgOpt.has_value()) {
            GTEST_SKIP() << "Skipping Cassandra tests: DEARSQL_TEST_CASSANDRA_HOST not set. "
                         << "Run scripts/run-tests to launch Docker test databases.";
        }

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::CASSANDRA;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = true;
        connInfo.sslmode = SslMode::Disable;

        database = std::make_shared<CassandraDatabase>(connInfo);

        // Cassandra can take a while to come up under Docker; retry connect.
        bool connected = false;
        std::string lastError;
        for (int attempt = 0; attempt < 60; ++attempt) {
            const auto [success, error] = database->connect();
            if (success) {
                connected = true;
                break;
            }
            lastError = error;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        ASSERT_TRUE(connected) << "Cassandra connection failed: " << lastError;

        keyspace_ = TestHelpers::makeUniqueIdentifier("dearsql_cass_test_");
        cleanup();
    }

    void TearDown() override {
        cleanup();
        if (database) {
            database->disconnect();
            database.reset();
        }
    }

    void cleanup() {
        if (database && !keyspace_.empty()) {
            database->dropDatabase(keyspace_);
        }
    }

    CassandraConfig config;
    std::shared_ptr<CassandraDatabase> database;
    std::string keyspace_;
};

TEST_F(CassandraDatabaseIntegrationTest, ConnectSuccessfully) {
    ASSERT_NE(database, nullptr);
    ASSERT_TRUE(database->isConnected());
}

TEST_F(CassandraDatabaseIntegrationTest, CreateAndDropKeyspace) {
    auto [okCreate, errCreate] = database->createDatabase(keyspace_);
    ASSERT_TRUE(okCreate) << errCreate;

    // The new keyspace should appear in the keyspace list.
    database->refreshDatabaseNames();
    // Drain the async loader synchronously by polling areDatabasesLoaded.
    for (int i = 0; i < 50 && !database->areDatabasesLoaded(); ++i) {
        database->checkDatabasesStatusAsync();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool found = false;
    for (const auto& [name, _] : database->getDatabaseDataMap()) {
        if (name == keyspace_) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);

    auto [okDrop, errDrop] = database->dropDatabase(keyspace_);
    EXPECT_TRUE(okDrop) << errDrop;
}

TEST_F(CassandraDatabaseIntegrationTest, ExecuteRawCqlInsertAndSelect) {
    auto [ok, err] = database->createDatabase(keyspace_);
    ASSERT_TRUE(ok) << err;

    const std::string table = "items";
    const std::string ddl =
        std::format("CREATE TABLE \"{}\".\"{}\" (id int PRIMARY KEY, name text)", keyspace_, table);
    auto ddlResult = database->executeQuery(ddl);
    ASSERT_TRUE(ddlResult.success()) << ddlResult.errorMessage();

    auto insert = database->executeQuery(
        std::format("INSERT INTO \"{}\".\"{}\" (id, name) VALUES (1, 'alpha')", keyspace_, table));
    ASSERT_TRUE(insert.success()) << insert.errorMessage();

    auto select = database->executeQuery(
        std::format("SELECT id, name FROM \"{}\".\"{}\" WHERE id = 1", keyspace_, table));
    ASSERT_TRUE(select.success()) << select.errorMessage();
    ASSERT_EQ(select.size(), 1u);
    EXPECT_EQ(select[0].columnNames.size(), 2u);
    ASSERT_EQ(select[0].tableData.size(), 1u);
    EXPECT_EQ(select[0].tableData[0][0], "1");
    EXPECT_EQ(select[0].tableData[0][1], "alpha");
}

TEST_F(CassandraDatabaseIntegrationTest, KeyspaceNodeListsTablesAndColumns) {
    auto [ok, err] = database->createDatabase(keyspace_);
    ASSERT_TRUE(ok) << err;

    const std::string table = "users";
    auto ddl = database->executeQuery(std::format(
        "CREATE TABLE \"{}\".\"{}\" (id uuid PRIMARY KEY, email text, age int)", keyspace_, table));
    ASSERT_TRUE(ddl.success()) << ddl.errorMessage();

    auto* node = database->getDatabaseData(keyspace_);
    ASSERT_NE(node, nullptr);

    node->startTablesLoadAsync(true);
    for (int i = 0; i < 50 && !node->tablesLoaded; ++i) {
        node->checkLoadingStatus();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(node->tablesLoaded);

    bool foundTable = false;
    bool foundIdPk = false;
    for (const auto& t : node->tables) {
        if (t.name == table) {
            foundTable = true;
            for (const auto& c : t.columns) {
                if (c.name == "id" && c.isPrimaryKey) {
                    foundIdPk = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundTable);
    EXPECT_TRUE(foundIdPk);
}

TEST_F(CassandraDatabaseIntegrationTest, RowCountReturnsExpectedValue) {
    auto [ok, err] = database->createDatabase(keyspace_);
    ASSERT_TRUE(ok) << err;

    const std::string table = "events";
    database->executeQuery(std::format(
        "CREATE TABLE \"{}\".\"{}\" (id int PRIMARY KEY, label text)", keyspace_, table));
    for (int i = 0; i < 5; ++i) {
        database->executeQuery(std::format(
            "INSERT INTO \"{}\".\"{}\" (id, label) VALUES ({}, 'e{}')", keyspace_, table, i, i));
    }

    auto* node = database->getDatabaseData(keyspace_);
    ASSERT_NE(node, nullptr);

    Table t;
    t.name = table;
    t.schema = keyspace_;
    EXPECT_EQ(node->getRowCount(t), 5);
}

// close() while catalog queries run on another thread must not free the
// session under them
TEST_F(CassandraDatabaseIntegrationTest, CloseDuringCatalogLoadIsSafe) {
    auto conn = database->libConnection();
    ASSERT_NE(conn, nullptr);
    auto db = conn->database("system_schema");
    ASSERT_NE(db, nullptr);

    std::atomic<bool> stop = false;
    std::thread loader([&] {
        while (!stop) {
            try {
                (void)db->tables();
            } catch (const std::exception&) {
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    conn->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop = true;
    loader.join();

    EXPECT_TRUE(db->tables().empty()); // closed: no session
}
