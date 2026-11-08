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
        connInfo.type =
            TestHelpers::typeFromEnv("DEARSQL_TEST_CASSANDRA_TYPE", DatabaseType::CASSANDRA);
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

// DESCRIBE runs on the server (cassandra 4+, scylladb): the table and its index
TEST_F(CassandraDatabaseIntegrationTest, TableDdlComesFromDescribe) {
    auto [ok, err] = database->createDatabase(keyspace_);
    ASSERT_TRUE(ok) << err;
    // one statement per call: cql has no batches of DDL
    for (const auto& sql :
         {std::format("CREATE TABLE \"{}\".t (id int PRIMARY KEY, v text)", keyspace_),
          std::format("CREATE INDEX t_v ON \"{}\".t (v)", keyspace_)}) {
        auto r = database->executeQuery(sql);
        ASSERT_TRUE(r.success()) << r.errorMessage();
    }
    auto* node = database->getDatabaseData(keyspace_);
    ASSERT_NE(node, nullptr);
    const auto ddl = node->withHandle([](dearsql::IDatabase& db) { return db.tableDdl("t"); });
    EXPECT_NE(ddl.find("CREATE TABLE"), std::string::npos) << ddl;
    EXPECT_NE(ddl.find("CREATE INDEX t_v"), std::string::npos) << ddl;
    // table options only come from the server's own DESCRIBE
    EXPECT_NE(ddl.find("gc_grace_seconds"), std::string::npos) << ddl;
}

// the tls port of the scylladb pass; cassandra's container has none
TEST(CassandraTls, EncryptsAndVerifiesTheServer) {
    const char* host = std::getenv("DEARSQL_TEST_CASSANDRA_HOST");
    const char* port = std::getenv("DEARSQL_TEST_CASSANDRA_TLS_PORT");
    const char* ca = std::getenv("DEARSQL_TEST_CA_CERT");
    if (!host || !port || !*port || !ca)
        GTEST_SKIP() << "no TLS port (DEARSQL_TEST_CASSANDRA_TLS_PORT, DEARSQL_TEST_CA_CERT)";
    // connects, then queries: pool connections verify too, not only the control one
    auto open = [&](SslMode mode, const std::string& caPath, const std::string& on = "") {
        DatabaseConnectionInfo info;
        info.type =
            TestHelpers::typeFromEnv("DEARSQL_TEST_CASSANDRA_TYPE", DatabaseType::CASSANDRA);
        info.host = on.empty() ? host : on;
        info.port = std::stoi(port);
        info.sslmode = mode;
        info.sslCACertPath = caPath;
        CassandraDatabase db(info);
        auto result = db.connect();
        if (result.first) {
            const auto r = db.executeQuery("SELECT release_version FROM system.local");
            if (!r.success())
                result = {false, r.errorMessage()};
        }
        db.disconnect();
        return result;
    };
    auto [encrypted, e1] = open(SslMode::Require, "");
    EXPECT_TRUE(encrypted) << e1;
    auto [verified, e2] = open(SslMode::VerifyCA, ca);
    EXPECT_TRUE(verified) << e2;
    // asking to verify without a CA must not quietly skip the check
    auto [noCa, e3] = open(SslMode::VerifyCA, "");
    EXPECT_FALSE(noCa);
    EXPECT_NE(e3.find("CA certificate"), std::string::npos) << e3;
    // verify host: the test cert names localhost and 127.0.0.1. a hostname is checked
    // against the reverse lookup of each node's ip (127.0.0.1 -> localhost), an ip as is
    auto [byName, e4] = open(SslMode::VerifyFull, ca, "localhost");
    EXPECT_TRUE(byName) << e4;
    auto [byIp, e5] = open(SslMode::VerifyFull, ca, "127.0.0.1");
    EXPECT_TRUE(byIp) << e5;
}
