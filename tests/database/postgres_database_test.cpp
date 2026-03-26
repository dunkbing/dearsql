#include "database/postgres/pg_table_dump.hpp"
#include "database/postgresql.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace {
    struct PostgresConfig {
        std::string name = "PostgresIntegration";
        std::string host;
        int port = 5432;
        std::string database;
        std::string user;
        std::string password;
    };

    std::optional<PostgresConfig> loadPostgresConfigFromEnv() {
        PostgresConfig cfg;
        const char* hostEnv = std::getenv("DEARSQL_TEST_PG_HOST");
        const char* portEnv = std::getenv("DEARSQL_TEST_PG_PORT");
        const char* databaseEnv = std::getenv("DEARSQL_TEST_PG_DB");
        const char* userEnv = std::getenv("DEARSQL_TEST_PG_USER");
        const char* passwordEnv = std::getenv("DEARSQL_TEST_PG_PASSWORD");
        const char* nameEnv = std::getenv("DEARSQL_TEST_PG_NAME");

        if (!hostEnv || !databaseEnv || !userEnv) {
            return std::nullopt;
        }

        cfg.host = hostEnv;
        cfg.database = databaseEnv;
        cfg.user = userEnv;
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

class PostgresDatabaseIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto cfgOpt = loadPostgresConfigFromEnv();
        ASSERT_TRUE(cfgOpt.has_value())
            << "Missing PostgreSQL test configuration. Run scripts/run_tests to launch "
            << "Docker test databases automatically, or set "
            << "DEARSQL_TEST_PG_HOST / DEARSQL_TEST_PG_DB / DEARSQL_TEST_PG_USER "
            << "(optional PORT/PASSWORD/NAME) before running the tests.";

        config = *cfgOpt;

        DatabaseConnectionInfo connInfo;
        connInfo.name = config.name;
        connInfo.type = DatabaseType::POSTGRESQL;
        connInfo.host = config.host;
        connInfo.port = config.port;
        connInfo.database = config.database;
        connInfo.username = config.user;
        connInfo.password = config.password;
        connInfo.showAllDatabases = false;

        database = std::make_shared<PostgresDatabase>(connInfo);
        bool connected = false;
        bool dbCreated = false;
        std::string lastError;
        for (int attempt = 0; attempt < 30; ++attempt) {
            try {
                const auto [success, error] = database->connect();
                if (success) {
                    connected = true;
                    break;
                }
                lastError = error;
            } catch (const std::exception& e) {
                lastError = e.what();
            }
            // Auto-create the test database if it doesn't exist
            if (!dbCreated && lastError.find("does not exist") != std::string::npos) {
                try {
                    auto adminInfo = connInfo;
                    adminInfo.database = "postgres";
                    auto admin = std::make_shared<PostgresDatabase>(adminInfo);
                    if (auto [ok, _] = admin->connect(); ok) {
                        admin->createDatabase(config.database);
                        admin->disconnect();
                        dbCreated = true;
                    }
                } catch (...) {
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        ASSERT_TRUE(connected) << "PostgreSQL connection failed: " << lastError;
        tableName = TestHelpers::makeUniqueIdentifier("dearsql_pg_test_");
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
        if (database && !tableName.empty()) {
            database->executeQuery(std::format(R"(DROP TABLE IF EXISTS "{}")", tableName));
        }
    }

    PostgresConfig config;
    std::shared_ptr<PostgresDatabase> database;
    std::string tableName;
};

TEST_F(PostgresDatabaseIntegrationTest, ExecuteQueryStructuredReadsInsertedRows) {
    ASSERT_NE(database, nullptr);
    ASSERT_FALSE(tableName.empty());

    auto r1 = database->executeQuery(std::format(
        R"(CREATE TABLE "{}" (id SERIAL PRIMARY KEY, value TEXT NOT NULL))", tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(
        std::format(R"(INSERT INTO "{}"(value) VALUES ('alpha'), ('beta'), ('gamma'))", tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto result =
        database->executeQuery(std::format(R"(SELECT value FROM "{}" ORDER BY id)", tableName));
    ASSERT_FALSE(result.empty());
    auto& stmt = result[0];
    ASSERT_TRUE(stmt.success) << stmt.errorMessage;
    ASSERT_EQ(stmt.columnNames.size(), 1u);
    EXPECT_EQ(stmt.columnNames[0], "value");

    // Verify we got data back
    EXPECT_FALSE(stmt.tableData.empty());
}

TEST_F(PostgresDatabaseIntegrationTest, DropCurrentlyConnectedDatabaseSwitchesToPostgresDatabase) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_pg_drop_active_");

    auto [created, createErr] = database->createDatabase(tempDb);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("permission denied") != std::string::npos ||
            err.find("must be superuser") != std::string::npos ||
            err.find("not enough privileges") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required for this test. Error: "
                         << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    DatabaseConnectionInfo tempConnInfo = database->getConnectionInfo();
    tempConnInfo.database = tempDb;
    auto activeDb = std::make_shared<PostgresDatabase>(tempConnInfo);

    const auto [connected, connectErr] = activeDb->connect();
    ASSERT_TRUE(connected) << connectErr;

    const auto [dropped, dropErr] = activeDb->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
    EXPECT_EQ(activeDb->getConnectionInfo().database, "postgres");

    auto verifyResult = database->executeQuery(
        std::format("SELECT datname FROM pg_database WHERE datname = '{}'", tempDb));
    ASSERT_TRUE(verifyResult.success()) << verifyResult.errorMessage();
    ASSERT_FALSE(verifyResult.empty());
    EXPECT_TRUE(verifyResult[0].tableData.empty());
}

TEST_F(PostgresDatabaseIntegrationTest, CreateDatabaseWithOptionsCreatesDatabaseAndComment) {
    ASSERT_NE(database, nullptr);

    const std::string tempDb = TestHelpers::makeUniqueIdentifier("dearsql_pg_create_opts_");

    CreateDatabaseOptions options;
    options.name = tempDb;
    options.owner = "";
    options.templateDb = "template1";
    options.encoding = "UTF8";
    options.tablespace = "pg_default";
    options.comment = "dearsql option test's comment";

    auto [created, createErr] = database->createDatabaseWithOptions(options);
    if (!created) {
        const std::string& err = createErr;
        if (err.find("permission denied") != std::string::npos ||
            err.find("must be superuser") != std::string::npos ||
            err.find("not enough privileges") != std::string::npos ||
            err.find("must be member of role") != std::string::npos) {
            GTEST_SKIP() << "Skipping: CREATE DATABASE privilege is required for this test. Error: "
                         << err;
        }
    }
    ASSERT_TRUE(created) << createErr;

    auto dbResult = database->executeQuery(
        std::format("SELECT datname, pg_catalog.pg_encoding_to_char(encoding) "
                    "FROM pg_database WHERE datname = '{}'",
                    tempDb));
    ASSERT_TRUE(dbResult.success()) << dbResult.errorMessage();
    ASSERT_FALSE(dbResult.empty());
    ASSERT_EQ(dbResult[0].tableData.size(), 1u);
    EXPECT_EQ(dbResult[0].tableData[0][0], tempDb);
    EXPECT_EQ(dbResult[0].tableData[0][1], "UTF8");

    auto commentResult = database->executeQuery(std::format(
        "SELECT shobj_description(oid, 'pg_database') FROM pg_database WHERE datname = '{}'",
        tempDb));
    ASSERT_TRUE(commentResult.success()) << commentResult.errorMessage();
    ASSERT_FALSE(commentResult.empty());
    ASSERT_EQ(commentResult[0].tableData.size(), 1u);
    EXPECT_EQ(commentResult[0].tableData[0][0], options.comment);

    const auto [dropped, dropErr] = database->dropDatabase(tempDb);
    ASSERT_TRUE(dropped) << dropErr;
}

// ========== Schema Node DDL Tests ==========

class PostgresSchemaNodeDDLTest : public PostgresDatabaseIntegrationTest {
protected:
    void SetUp() override {
        PostgresDatabaseIntegrationTest::SetUp();
        if (testing::Test::HasFatalFailure())
            return;

        dbNode = database->getDatabaseData(config.database);
        ASSERT_NE(dbNode, nullptr);

        schemaNode = std::make_unique<PostgresSchemaNode>();
        schemaNode->parentDbNode = dbNode;
        schemaNode->name = "public";

        schemaName = TestHelpers::makeUniqueIdentifier("dearsql_pg_schema_");
        viewName = TestHelpers::makeUniqueIdentifier("dearsql_pg_view_");
    }

    void TearDown() override {
        if (database) {
            database->executeQuery(std::format(R"(DROP TABLE IF EXISTS public."{}")", tableName));
            database->executeQuery(
                std::format(R"(DROP TABLE IF EXISTS public."{}")", tableName + "_renamed"));
            database->executeQuery(std::format(R"(DROP VIEW IF EXISTS public."{}")", viewName));
            database->executeQuery(
                std::format(R"(DROP SCHEMA IF EXISTS "{}" CASCADE)", schemaName));
            database->executeQuery(
                std::format(R"(DROP SCHEMA IF EXISTS "{}" CASCADE)", schemaName + "_new"));
        }
        schemaNode.reset();
        PostgresDatabaseIntegrationTest::TearDown();
    }

    PostgresDatabaseNode* dbNode = nullptr;
    std::unique_ptr<PostgresSchemaNode> schemaNode;
    std::string schemaName;
    std::string viewName;
};

TEST_F(PostgresSchemaNodeDDLTest, RenameTableRenamesSuccessfully) {
    auto r = database->executeQuery(
        std::format(R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, val TEXT))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    const std::string newName = tableName + "_renamed";
    auto [ok, err] = schemaNode->renameTable(tableName, newName);
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(std::format(
        "SELECT tablename FROM pg_tables WHERE schemaname='public' AND tablename='{}'", newName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData.size(), 1u);
}

TEST_F(PostgresSchemaNodeDDLTest, DropTableRemovesTable) {
    auto r = database->executeQuery(
        std::format(R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = schemaNode->dropTable(tableName);
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(std::format(
        "SELECT tablename FROM pg_tables WHERE schemaname='public' AND tablename='{}'", tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_TRUE(check[0].tableData.empty());
}

TEST_F(PostgresSchemaNodeDDLTest, TruncateTableRemovesAllRows) {
    auto r = database->executeQuery(
        std::format(R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, val TEXT))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto ins = database->executeQuery(
        std::format(R"(INSERT INTO public."{}" (val) VALUES ('a'), ('b'), ('c'))", tableName));
    ASSERT_TRUE(ins.success()) << ins.errorMessage();

    auto pre =
        database->executeQuery(std::format(R"(SELECT COUNT(*) FROM public."{}")", tableName));
    ASSERT_TRUE(pre.success());
    ASSERT_FALSE(pre.empty());
    EXPECT_EQ(pre[0].tableData[0][0], "3");

    auto [ok, err] = schemaNode->truncateTable(tableName);
    ASSERT_TRUE(ok) << err;

    auto check =
        database->executeQuery(std::format(R"(SELECT COUNT(*) FROM public."{}")", tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData[0][0], "0");
}

TEST_F(PostgresSchemaNodeDDLTest, DropColumnRemovesColumn) {
    auto r = database->executeQuery(std::format(
        R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, keep_me TEXT, drop_me TEXT))",
        tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto [ok, err] = schemaNode->dropColumn(tableName, "drop_me");
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT column_name FROM information_schema.columns "
                    "WHERE table_schema='public' AND table_name='{}' ORDER BY ordinal_position",
                    tableName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    ASSERT_EQ(check[0].tableData.size(), 2u);
    EXPECT_EQ(check[0].tableData[0][0], "id");
    EXPECT_EQ(check[0].tableData[1][0], "keep_me");
}

TEST_F(PostgresSchemaNodeDDLTest, DropViewRemovesView) {
    auto r1 = database->executeQuery(
        std::format(R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, val TEXT))", tableName));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(std::format(
        R"(CREATE VIEW public."{}" AS SELECT val FROM public."{}")", viewName, tableName));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    auto [ok, err] = schemaNode->dropView(viewName);
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(std::format(
        "SELECT viewname FROM pg_views WHERE schemaname='public' AND viewname='{}'", viewName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_TRUE(check[0].tableData.empty());
}

TEST_F(PostgresSchemaNodeDDLTest, RenameSchemaRenamesSuccessfully) {
    auto r = database->executeQuery(std::format(R"(CREATE SCHEMA "{}")", schemaName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto testSchema = std::make_unique<PostgresSchemaNode>();
    testSchema->parentDbNode = dbNode;
    testSchema->name = schemaName;

    const std::string newSchemaName = schemaName + "_new";
    auto [ok, err] = testSchema->renameSchema(newSchemaName);
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT nspname FROM pg_namespace WHERE nspname='{}'", newSchemaName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_EQ(check[0].tableData.size(), 1u);
}

TEST_F(PostgresSchemaNodeDDLTest, DropSchemaRemovesSchema) {
    auto r = database->executeQuery(std::format(R"(CREATE SCHEMA "{}")", schemaName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto testSchema = std::make_unique<PostgresSchemaNode>();
    testSchema->parentDbNode = dbNode;
    testSchema->name = schemaName;

    auto [ok, err] = testSchema->dropSchema();
    ASSERT_TRUE(ok) << err;

    auto check = database->executeQuery(
        std::format("SELECT nspname FROM pg_namespace WHERE nspname='{}'", schemaName));
    ASSERT_TRUE(check.success());
    ASSERT_FALSE(check.empty());
    EXPECT_TRUE(check[0].tableData.empty());
}

// ========== Table Dump Tests ==========

TEST_F(PostgresSchemaNodeDDLTest, GenerateCreateTableProducesValidDDL) {
    auto r = database->executeQuery(std::format(
        R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, name TEXT NOT NULL DEFAULT 'unknown', score INTEGER))",
        tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    Table table;
    table.name = tableName;
    table.columns = {
        {"id", "integer", "nextval('\"" + tableName + "_id_seq\"'::regclass)", "", true, true, false},
        {"name", "text", "'unknown'", "", false, true, false},
        {"score", "integer", "", "", false, false, false},
    };
    table.indexes = {
        {tableName + "_pkey", {"id"}, false, true, "btree"},
    };

    std::string ddl = PgTableDump::generateCreateTable(table, "public");

    EXPECT_NE(ddl.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(ddl.find("\"id\""), std::string::npos);
    EXPECT_NE(ddl.find("\"name\""), std::string::npos);
    EXPECT_NE(ddl.find("NOT NULL"), std::string::npos);
    EXPECT_NE(ddl.find("PRIMARY KEY"), std::string::npos);
    EXPECT_NE(ddl.find("DEFAULT"), std::string::npos);
}

TEST_F(PostgresSchemaNodeDDLTest, DumpTableWritesDDLAndData) {
    auto r = database->executeQuery(std::format(
        R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, val TEXT NOT NULL))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto ins = database->executeQuery(
        std::format(R"(INSERT INTO public."{}" (val) VALUES ('alpha'), ('beta'), ('gamma'))",
                    tableName));
    ASSERT_TRUE(ins.success()) << ins.errorMessage();

    Table table;
    table.name = tableName;
    table.columns = {
        {"id", "integer", "", "", true, true, false},
        {"val", "text", "", "", false, true, false},
    };
    table.indexes = {
        {tableName + "_pkey", {"id"}, false, true, "btree"},
    };

    auto outputPath =
        std::filesystem::temp_directory_path() / (tableName + "_dump.sql");
    auto session = dbNode->getSession();
    bool ok = PgTableDump::dumpTable(session.get(), table, "public", outputPath.string());
    ASSERT_TRUE(ok);

    std::ifstream file(outputPath);
    ASSERT_TRUE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    std::filesystem::remove(outputPath);

    // DDL present
    EXPECT_NE(content.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(content.find("PRIMARY KEY"), std::string::npos);

    // COPY header and terminator present
    EXPECT_NE(content.find("COPY"), std::string::npos);
    EXPECT_NE(content.find("FROM stdin"), std::string::npos);
    EXPECT_NE(content.find("\\."), std::string::npos);

    // data rows present
    EXPECT_NE(content.find("alpha"), std::string::npos);
    EXPECT_NE(content.find("beta"), std::string::npos);
    EXPECT_NE(content.find("gamma"), std::string::npos);
}

TEST_F(PostgresSchemaNodeDDLTest, DumpEmptyTableWritesDDLOnly) {
    auto r = database->executeQuery(std::format(
        R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, val TEXT))", tableName));
    ASSERT_TRUE(r.success()) << r.errorMessage();

    Table table;
    table.name = tableName;
    table.columns = {
        {"id", "integer", "", "", true, true, false},
        {"val", "text", "", "", false, false, false},
    };
    table.indexes = {
        {tableName + "_pkey", {"id"}, false, true, "btree"},
    };

    auto outputPath =
        std::filesystem::temp_directory_path() / (tableName + "_empty_dump.sql");
    auto session = dbNode->getSession();
    bool ok = PgTableDump::dumpTable(session.get(), table, "public", outputPath.string());
    ASSERT_TRUE(ok);

    std::ifstream file(outputPath);
    ASSERT_TRUE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();
    std::filesystem::remove(outputPath);

    EXPECT_NE(content.find("CREATE TABLE"), std::string::npos);
    EXPECT_NE(content.find("COPY"), std::string::npos);
    EXPECT_NE(content.find("\\."), std::string::npos);
}

TEST_F(PostgresSchemaNodeDDLTest, DumpTableWithForeignKeysIncludesConstraints) {
    const std::string parentTable = tableName + "_parent";
    auto r1 = database->executeQuery(std::format(
        R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, name TEXT))", parentTable));
    ASSERT_TRUE(r1.success()) << r1.errorMessage();

    auto r2 = database->executeQuery(std::format(
        R"(CREATE TABLE public."{}" (id SERIAL PRIMARY KEY, parent_id INTEGER REFERENCES public."{}"(id) ON DELETE CASCADE))",
        tableName, parentTable));
    ASSERT_TRUE(r2.success()) << r2.errorMessage();

    Table table;
    table.name = tableName;
    table.columns = {
        {"id", "integer", "", "", true, true, false},
        {"parent_id", "integer", "", "", false, false, false},
    };
    table.indexes = {
        {tableName + "_pkey", {"id"}, false, true, "btree"},
    };
    table.foreignKeys = {
        {tableName + "_parent_id_fkey", "parent_id", parentTable, "id", "CASCADE", "NO ACTION"},
    };

    std::string ddl = PgTableDump::generateCreateTable(table, "public");

    EXPECT_NE(ddl.find("FOREIGN KEY"), std::string::npos);
    EXPECT_NE(ddl.find("REFERENCES"), std::string::npos);
    EXPECT_NE(ddl.find("ON DELETE CASCADE"), std::string::npos);

    // cleanup parent table
    database->executeQuery(std::format(R"(DROP TABLE IF EXISTS public."{}" CASCADE)", parentTable));
}
