// verifies legacy rows (shipped-constant key or plaintext) are re-encrypted
// with the keystore-backed master secret on startup, seamlessly for the user.
#include "app_state.hpp"
#include "config.hpp"
#include "utils/crypto.hpp"
#include "utils/master_secret.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

    void setEnvVar(const char* key, const std::string& value) {
#ifdef _WIN32
        _putenv_s(key, value.c_str());
#else
        setenv(key, value.c_str(), 1);
#endif
    }

    std::string b64(const std::string& s) {
        return CryptoUtils::base64Encode(std::vector<uint8_t>(s.begin(), s.end()));
    }

} // namespace

// single test: MasterSecret::get() caches on first use, so env must be set once
TEST(CredentialMigration, LegacyRowsAreReencryptedWithMasterKey) {
    const fs::path tmp = fs::temp_directory_path() / "dearsql_migration_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / ".dearsql");
    // redirect HOME so AppState uses the temp db; restored below for other tests
    const char* oldHome = std::getenv("HOME");
    const char* oldProfile = std::getenv("USERPROFILE");
    const std::string savedHome = oldHome ? oldHome : "";
    const std::string savedProfile = oldProfile ? oldProfile : "";
    setEnvVar("HOME", tmp.string());
    setEnvVar("USERPROFILE", tmp.string());

    // legacy db: pre-key_version schema, one row encrypted with the shipped
    // constant, one pre-encryption plaintext row (no salt)
    const std::string dbFile = (tmp / ".dearsql" / "connections.db").string();
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(dbFile.c_str(), &db));
        ASSERT_EQ(SQLITE_OK, sqlite3_exec(db, R"(
            CREATE TABLE saved_connections (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL, type TEXT NOT NULL,
                host TEXT, port INTEGER, database_name TEXT,
                username TEXT, password TEXT, path TEXT, salt TEXT,
                last_used DATETIME DEFAULT CURRENT_TIMESTAMP,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            );)",
                                          nullptr, nullptr, nullptr));

        const std::string salt = CryptoUtils::generateSalt();
        const std::string legacyKey = CryptoUtils::deriveKey(CREDS_SECRET, salt);
        const std::string sql =
            "INSERT INTO saved_connections (name, type, host, port, username, password, salt) "
            "VALUES ('enc', 'postgresql', 'localhost', 5432, ?, ?, ?), "
            "('plain', 'mysql', 'localhost', 3306, 'bob', 'plaintext-pw', NULL);";
        sqlite3_stmt* stmt = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr));
        const std::string encUser = CryptoUtils::encrypt("alice", legacyKey);
        const std::string encPass = CryptoUtils::encrypt("s3cret", legacyKey);
        const std::string saltB64 = b64(salt);
        sqlite3_bind_text(stmt, 1, encUser.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, encPass.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, saltB64.c_str(), -1, SQLITE_TRANSIENT);
        ASSERT_EQ(SQLITE_DONE, sqlite3_step(stmt));
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    // startup runs the migration
    AppState state;
    ASSERT_TRUE(state.initialize());

    auto conns = state.getSavedConnections();
    ASSERT_EQ(2u, conns.size());
    for (const auto& conn : conns) {
        if (conn.connectionInfo.name == "enc") {
            EXPECT_EQ("alice", conn.connectionInfo.username);
            EXPECT_EQ("s3cret", conn.connectionInfo.password);
        } else {
            EXPECT_EQ("bob", conn.connectionInfo.username);
            EXPECT_EQ("plaintext-pw", conn.connectionInfo.password);
        }
    }

    // a fresh install keeps the key in a file, never the keychain
    EXPECT_FALSE(MasterSecret::usingKeychain());

    // rows are now on the master key, not the legacy key
    std::string masterSecret;
    {
        std::ifstream in(tmp / ".dearsql" / "master.key");
        ASSERT_TRUE(bool(in));
        std::getline(in, masterSecret);
        ASSERT_FALSE(masterSecret.empty());
    }
    {
        sqlite3* db = nullptr;
        ASSERT_EQ(SQLITE_OK, sqlite3_open(dbFile.c_str(), &db));
        sqlite3_stmt* stmt = nullptr;
        ASSERT_EQ(SQLITE_OK,
                  sqlite3_prepare_v2(db,
                                     "SELECT password, salt, key_version FROM saved_connections "
                                     "WHERE name = 'enc';",
                                     -1, &stmt, nullptr));
        ASSERT_EQ(SQLITE_ROW, sqlite3_step(stmt));
        const std::string storedPass = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const std::string storedSaltB64 =
            reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        EXPECT_EQ(1, sqlite3_column_int(stmt, 2));
        sqlite3_finalize(stmt);
        sqlite3_close(db);

        auto saltData = CryptoUtils::base64Decode(storedSaltB64);
        const std::string salt(saltData.begin(), saltData.end());
        EXPECT_EQ("s3cret",
                  CryptoUtils::decrypt(storedPass, CryptoUtils::deriveKey(masterSecret, salt)));
        EXPECT_THROW(CryptoUtils::decrypt(storedPass, CryptoUtils::deriveKey(CREDS_SECRET, salt)),
                     std::exception);
    }

#ifdef __APPLE__
    // apple: the rows moved to the core data store with their ids, so scripts and
    // chats keyed by connection id stay linked; a second start doesn't migrate again
    int encId = 0;
    for (const auto& conn : conns) {
        EXPECT_EQ(conn.connectionInfo.name == "enc" ? 1 : 2, conn.id);
        if (conn.connectionInfo.name == "enc")
            encId = conn.id;
    }
    SqlScript script;
    script.name = "q";
    script.filePath = (tmp / "q.sql").string();
    script.connectionId = encId;
    ASSERT_GT(state.saveScript(script), 0);
    ASSERT_TRUE(state.setSetting("theme", "synced-theme"));

    SavedConnection added;
    added.connectionInfo.name = "added";
    added.connectionInfo.type = DatabaseType::POSTGRESQL;
    added.connectionInfo.password = "new-pw";
    const int addedId = state.saveConnection(added);
    EXPECT_EQ(3, addedId); // past every id the old table used
    ASSERT_TRUE(state.renameConnection(encId, "renamed"));
    {
        AppState again;
        ASSERT_TRUE(again.initialize());
        const auto reloaded = again.getSavedConnections();
        ASSERT_EQ(3u, reloaded.size());
        for (const auto& conn : reloaded) {
            if (conn.id == encId) {
                EXPECT_EQ("renamed", conn.connectionInfo.name);
                EXPECT_EQ("s3cret", conn.connectionInfo.password);
            }
            if (conn.id == addedId)
                EXPECT_EQ("new-pw", conn.connectionInfo.password);
        }
        EXPECT_EQ(1u, again.getScriptsForConnection(encId).size());
        EXPECT_EQ("synced-theme", again.getSetting("theme"));
        EXPECT_EQ(1u, again.getWorkspaces().size());
    }
    EXPECT_TRUE(fs::exists(tmp / ".dearsql" / "connections-before-sync.db"));
#endif

    setEnvVar("HOME", savedHome);
    setEnvVar("USERPROFILE", savedProfile);
    fs::remove_all(tmp);
}
