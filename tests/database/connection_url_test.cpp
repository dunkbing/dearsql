#include "database/connection_url.hpp"

#include <gtest/gtest.h>

TEST(ConnectionUrlTest, ParsesPostgresqlBasic) {
    auto r = parseConnectionUrl("postgresql://alice:s3cret@db.example.com:5433/app");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.type, DatabaseType::POSTGRESQL);
    EXPECT_EQ(r.info.host, "db.example.com");
    EXPECT_EQ(r.info.port, 5433);
    EXPECT_EQ(r.info.username, "alice");
    EXPECT_EQ(r.info.password, "s3cret");
    EXPECT_EQ(r.info.database, "app");
}

TEST(ConnectionUrlTest, PostgresAliasAndDefaultPort) {
    auto r = parseConnectionUrl("postgres://localhost/postgres");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.type, DatabaseType::POSTGRESQL);
    EXPECT_EQ(r.info.host, "localhost");
    EXPECT_EQ(r.info.port, 5432);
    EXPECT_EQ(r.info.database, "postgres");
    EXPECT_TRUE(r.info.username.empty());
    EXPECT_TRUE(r.info.password.empty());
}

TEST(ConnectionUrlTest, MysqlAndMariadb) {
    {
        auto r = parseConnectionUrl("mysql://root@127.0.0.1:3307/sakila");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::MYSQL);
        EXPECT_EQ(r.info.host, "127.0.0.1");
        EXPECT_EQ(r.info.port, 3307);
        EXPECT_EQ(r.info.username, "root");
        EXPECT_EQ(r.info.database, "sakila");
    }
    {
        auto r = parseConnectionUrl("mariadb://u:p@m.example.com/test");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::MARIADB);
        EXPECT_EQ(r.info.port, 3306);
    }
}

TEST(ConnectionUrlTest, MongoBasicAndSrv) {
    {
        auto r = parseConnectionUrl("mongodb://app:p%40ss@cluster.example.com:27018/orders");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::MONGODB);
        EXPECT_EQ(r.info.host, "cluster.example.com");
        EXPECT_EQ(r.info.port, 27018);
        EXPECT_EQ(r.info.username, "app");
        EXPECT_EQ(r.info.password, "p@ss");
        EXPECT_EQ(r.info.database, "orders");
        EXPECT_EQ(r.info.sslmode, SslMode::Disable);
    }
    {
        auto r = parseConnectionUrl("mongodb+srv://srv.example.com/db");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::MONGODB);
    }
}

TEST(ConnectionUrlTest, RedisAndRediss) {
    {
        auto r = parseConnectionUrl("redis://:pw@cache.local:6380");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::REDIS);
        EXPECT_EQ(r.info.host, "cache.local");
        EXPECT_EQ(r.info.port, 6380);
        EXPECT_TRUE(r.info.username.empty());
        EXPECT_EQ(r.info.password, "pw");
        EXPECT_EQ(r.info.sslmode, SslMode::Disable);
    }
    {
        auto r = parseConnectionUrl("rediss://cache.local");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::REDIS);
        EXPECT_EQ(r.info.port, 6379);
        EXPECT_EQ(r.info.sslmode, SslMode::Require);
    }
}

TEST(ConnectionUrlTest, MssqlAndOracleAndRedshift) {
    {
        auto r = parseConnectionUrl("sqlserver://sa:Strong!@db:1433/master");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::MSSQL);
    }
    {
        auto r = parseConnectionUrl("oracle://scott:tiger@host:1521/ORCL");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::ORACLE);
        EXPECT_EQ(r.info.database, "ORCL");
    }
    {
        auto r = parseConnectionUrl("redshift://u:p@cluster.region.redshift.amazonaws.com/dev");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::REDSHIFT);
        EXPECT_EQ(r.info.port, 5439);
    }
}

TEST(ConnectionUrlTest, Sqlite) {
    {
        auto r = parseConnectionUrl("sqlite:///tmp/data.db");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::SQLITE);
        EXPECT_EQ(r.info.path, "/tmp/data.db");
    }
    {
        auto r = parseConnectionUrl("sqlite://relative.db");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.path, "relative.db");
    }
    {
        auto r = parseConnectionUrl("sqlite://C%3A/data/db%20with%20space.db");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.path, "C:/data/db with space.db");
    }
}

TEST(ConnectionUrlTest, PercentDecodesCredentialsAndDatabase) {
    auto r = parseConnectionUrl("postgresql://us%40r:p%26w%2Fd@host/sales%20db");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.username, "us@r");
    EXPECT_EQ(r.info.password, "p&w/d");
    EXPECT_EQ(r.info.database, "sales db");
}

TEST(ConnectionUrlTest, Ipv6Host) {
    auto r = parseConnectionUrl("postgresql://u:p@[::1]:6000/app");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.host, "::1");
    EXPECT_EQ(r.info.port, 6000);
}

TEST(ConnectionUrlTest, QueryParamsSslmodeAndCa) {
    auto r =
        parseConnectionUrl("postgresql://u:p@h/db?sslmode=verify-full&sslrootcert=/etc/ca.pem");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.sslmode, SslMode::VerifyFull);
    EXPECT_EQ(r.info.sslCACertPath, "/etc/ca.pem");
}

TEST(ConnectionUrlTest, MongoTlsQueryParam) {
    auto r = parseConnectionUrl("mongodb://u:p@h/db?tls=true&tlsCAFile=/ca.pem");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.sslmode, SslMode::Require);
    EXPECT_EQ(r.info.sslCACertPath, "/ca.pem");
}

TEST(ConnectionUrlTest, NoUserinfoOrDatabase) {
    auto r = parseConnectionUrl("postgresql://only.host");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.host, "only.host");
    EXPECT_EQ(r.info.port, 5432);
    EXPECT_TRUE(r.info.database.empty());
    EXPECT_TRUE(r.info.username.empty());
    EXPECT_TRUE(r.info.password.empty());
}

TEST(ConnectionUrlTest, CaseInsensitiveScheme) {
    auto r = parseConnectionUrl("PostgreSQL://h/db");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.type, DatabaseType::POSTGRESQL);
}

TEST(ConnectionUrlTest, TrimsWhitespace) {
    auto r = parseConnectionUrl("   postgresql://h/db\n");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.host, "h");
}

TEST(ConnectionUrlTest, AtSignInPasswordSplitOnRightmost) {
    // password contains literal '@' character via percent-encoding; rightmost '@'
    // separates userinfo from host.
    auto r = parseConnectionUrl("mysql://u:a%40b@host/db");
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.info.username, "u");
    EXPECT_EQ(r.info.password, "a@b");
    EXPECT_EQ(r.info.host, "host");
}

// -- error cases --

TEST(ConnectionUrlTest, RejectsMissingScheme) {
    auto r = parseConnectionUrl("host:5432/db");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(ConnectionUrlTest, RejectsUnknownScheme) {
    auto r = parseConnectionUrl("ftp://example.com/db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsMissingHost) {
    auto r = parseConnectionUrl("postgresql:///db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsInvalidPort) {
    auto r = parseConnectionUrl("postgresql://h:notanum/db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsPartiallyNumericPort) {
    auto r = parseConnectionUrl("postgresql://h:5432abc/db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsPartiallyNumericIpv6Port) {
    auto r = parseConnectionUrl("postgresql://[::1]:5432abc/db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsPortOutOfRange) {
    auto r = parseConnectionUrl("postgresql://h:70000/db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsMalformedPercentEscape) {
    auto r = parseConnectionUrl("postgresql://u:p%ZZ@h/db");
    EXPECT_FALSE(r.ok);
}

TEST(ConnectionUrlTest, RejectsUnterminatedIpv6) {
    auto r = parseConnectionUrl("postgresql://u@[::1/db");
    EXPECT_FALSE(r.ok);
}

// -- helper --

TEST(ConnectionUrlTest, BuildPostgresBasic) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.host = "db.example.com";
    info.port = 5433;
    info.username = "alice";
    info.password = "s3cret";
    info.database = "app";
    info.sslmode = SslMode::Prefer;
    EXPECT_EQ(buildConnectionUrl(info), "postgresql://alice:s3cret@db.example.com:5433/app");
}

TEST(ConnectionUrlTest, BuildAlwaysIncludesPort) {
    // Port is always emitted (even when it equals the scheme default) so the
    // URL stays in sync with the Port field in the connection dialog.
    DatabaseConnectionInfo info;
    info.type = DatabaseType::MYSQL;
    info.host = "h";
    info.port = 3306;
    info.username = "u";
    info.password = "p";
    info.database = "d";
    info.sslmode = SslMode::Prefer;
    EXPECT_EQ(buildConnectionUrl(info), "mysql://u:p@h:3306/d");
}

TEST(ConnectionUrlTest, BuildPercentEncodesCredentialsAndDb) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.host = "h";
    info.port = 5432;
    info.username = "us@r";
    info.password = "p&w/d";
    info.database = "sales db";
    info.sslmode = SslMode::Prefer;
    EXPECT_EQ(buildConnectionUrl(info), "postgresql://us%40r:p%26w%2Fd@h:5432/sales%20db");
}

TEST(ConnectionUrlTest, BuildSqlite) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.path = "/tmp/data.db";
    EXPECT_EQ(buildConnectionUrl(info), "sqlite:////tmp/data.db");
}

TEST(ConnectionUrlTest, BuildRedissForTls) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::REDIS;
    info.host = "cache";
    info.port = 6379;
    info.sslmode = SslMode::Require;
    EXPECT_EQ(buildConnectionUrl(info), "rediss://cache:6379");
}

TEST(ConnectionUrlTest, BuildAddsSslmodeWhenNotDefault) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.host = "h";
    info.port = 5432;
    info.sslmode = SslMode::VerifyFull;
    info.sslCACertPath = "/etc/ca.pem";
    EXPECT_EQ(buildConnectionUrl(info),
              "postgresql://h:5432?sslmode=verify-full&sslrootcert=%2Fetc%2Fca.pem");
}

TEST(ConnectionUrlTest, BuildIpv6Brackets) {
    DatabaseConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.host = "::1";
    info.port = 6000;
    info.sslmode = SslMode::Prefer;
    EXPECT_EQ(buildConnectionUrl(info), "postgresql://[::1]:6000");
}

TEST(ConnectionUrlTest, RoundTripParseBuildPostgres) {
    auto r = parseConnectionUrl("postgresql://alice:s3cret@db.example.com:5433/app");
    ASSERT_TRUE(r.ok);
    std::string rebuilt = buildConnectionUrl(r.info);
    auto r2 = parseConnectionUrl(rebuilt);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r2.info.host, r.info.host);
    EXPECT_EQ(r2.info.port, r.info.port);
    EXPECT_EQ(r2.info.username, r.info.username);
    EXPECT_EQ(r2.info.password, r.info.password);
    EXPECT_EQ(r2.info.database, r.info.database);
    EXPECT_EQ(r2.info.type, r.info.type);
}

TEST(ConnectionUrlTest, RoundTripWithSpecialChars) {
    auto r = parseConnectionUrl("mysql://us%40r:p%40ss@h:3306/d%20b");
    ASSERT_TRUE(r.ok);
    std::string rebuilt = buildConnectionUrl(r.info);
    auto r2 = parseConnectionUrl(rebuilt);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(r2.info.username, "us@r");
    EXPECT_EQ(r2.info.password, "p@ss");
    EXPECT_EQ(r2.info.database, "d b");
}

TEST(ConnectionUrlTest, LooksLikeConnectionUrlDetectsKnownSchemes) {
    EXPECT_TRUE(looksLikeConnectionUrl("postgresql://h/db"));
    EXPECT_TRUE(looksLikeConnectionUrl("REDIS://h"));
    EXPECT_TRUE(looksLikeConnectionUrl("sqlite:///x.db"));
    EXPECT_FALSE(looksLikeConnectionUrl("just a host name"));
    EXPECT_FALSE(looksLikeConnectionUrl("http://example.com"));
    EXPECT_FALSE(looksLikeConnectionUrl(""));
}

TEST(ConnectionUrlTest, CompatibleEngines) {
    {
        auto r = parseConnectionUrl("tidb://root@gateway01.us-west-2.prod.aws.tidbcloud.com/test");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::TIDB);
        EXPECT_EQ(r.info.port, 4000);
        EXPECT_EQ(r.info.database, "test");
    }
    {
        auto r = parseConnectionUrl("cockroachdb://root@localhost/defaultdb");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::COCKROACHDB);
        EXPECT_EQ(r.info.port, 26257);
    }
    {
        auto r = parseConnectionUrl("cockroach://u:p@h:26258/bank");
        ASSERT_TRUE(r.ok) << r.error;
        EXPECT_EQ(r.info.type, DatabaseType::COCKROACHDB);
        EXPECT_EQ(r.info.port, 26258);
        DatabaseConnectionInfo round = r.info;
        EXPECT_EQ(parseConnectionUrl(buildConnectionUrl(round)).info.type,
                  DatabaseType::COCKROACHDB);
    }
}
