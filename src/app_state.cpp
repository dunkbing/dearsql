#include "app_state.hpp"
#include "config.hpp"
#include "utils/app_paths.hpp"
#include "utils/crypto.hpp"
#include "utils/master_secret.hpp"
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <spdlog/spdlog.h>
#include <sqlite3.h>
#include <sstream>

#ifdef __APPLE__
#include "utils/synced_store.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>
// apple: connections, workspaces and synced settings go to the store (bottom of file)
#define VIA_STORE(cond, ...)                                                                       \
    if (store_ && (cond))                                                                          \
    return __VA_ARGS__
#else
#define VIA_STORE(cond, ...)
#endif

namespace fs = std::filesystem;

namespace {

    // ios moves the app container on every install: point saved files at the current one
    void relocate(DatabaseConnectionInfo& i) {
#ifdef DEARSQL_MOBILE
        const char* home = std::getenv("DEARSQL_CONTAINER");
        for (std::string* p : {&i.path, &i.sslCACertPath, &i.ssh.privateKeyPath}) {
            const auto at = p->find("/Data/Application/");
            const auto end = at == std::string::npos ? at : p->find('/', at + 18);
            if (home && end != std::string::npos)
                *p = home + p->substr(end);
        }
#else
        (void)i;
#endif
    }

    // RAII wrapper for sqlite3_stmt
    struct StmtDeleter {
        void operator()(sqlite3_stmt* stmt) const {
            if (stmt)
                sqlite3_finalize(stmt);
        }
    };
    using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

    // Return column text as string, or "NULL" if the column is null
    std::string columnText(sqlite3_stmt* stmt, int col) {
        if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
            return "NULL";
        }
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
        return text ? std::string(text) : "NULL";
    }

    // base secret for a row's key derivation; key_version 0 = legacy shipped constant
    std::string baseSecretForVersion(const int keyVersion) {
        if (keyVersion == 1) {
            if (const std::string& master = MasterSecret::get(); !master.empty()) {
                return master;
            }
        }
        return CREDS_SECRET;
    }

    // secret + key_version to use when writing rows
    std::pair<std::string, int> writeSecret() {
        const std::string& master = MasterSecret::get();
        if (master.empty()) {
            return {CREDS_SECRET, 0};
        }
        return {master, 1};
    }

    // Parse a row from the saved_connections query into a SavedConnection
    bool parseConnectionRow(sqlite3_stmt* stmt, SavedConnection& conn) {
        conn.id = sqlite3_column_int(stmt, 0);
        conn.connectionInfo.name = columnText(stmt, 1);

        std::string typeStr = columnText(stmt, 2);
        conn.connectionInfo.type = dearsql::stringToDatabaseType(typeStr);
        if (dearsql::databaseTypeToString(conn.connectionInfo.type) != typeStr) {
            spdlog::warn("Unknown database type '{}' for connection '{}', skipping", typeStr,
                         conn.connectionInfo.name);
            return false;
        }

        std::string hostStr = columnText(stmt, 3);
        conn.connectionInfo.host = (hostStr == "NULL") ? "" : hostStr;

        std::string portStr = columnText(stmt, 4);
        conn.connectionInfo.port = (portStr == "NULL" || portStr.empty()) ? 0 : std::stoi(portStr);

        std::string dbStr = columnText(stmt, 5);
        conn.connectionInfo.database = (dbStr == "NULL") ? "" : dbStr;

        std::string encryptedUsername = columnText(stmt, 6);
        if (encryptedUsername == "NULL")
            encryptedUsername = "";

        std::string encryptedPassword = columnText(stmt, 7);
        if (encryptedPassword == "NULL")
            encryptedPassword = "";

        std::string saltStr = columnText(stmt, 9);
        if (saltStr == "NULL")
            saltStr = "";

        const int keyVersion = sqlite3_column_int(stmt, 22);
        conn.connectionInfo.readOnly = sqlite3_column_int(stmt, 23) != 0;

        // Derive encryption key once for both DB and SSH credentials
        std::string encryptionKey;
        try {
            if (!saltStr.empty()) {
                auto saltData = CryptoUtils::base64Decode(saltStr);
                std::string salt(saltData.begin(), saltData.end());
                encryptionKey = CryptoUtils::deriveKey(baseSecretForVersion(keyVersion), salt);

                if (!encryptedUsername.empty()) {
                    try {
                        conn.connectionInfo.username =
                            CryptoUtils::decrypt(encryptedUsername, encryptionKey);
                    } catch (...) {
                        conn.connectionInfo.username = "";
                    }
                }

                if (!encryptedPassword.empty()) {
                    try {
                        conn.connectionInfo.password =
                            CryptoUtils::decrypt(encryptedPassword, encryptionKey);
                    } catch (...) {
                        conn.connectionInfo.password = "";
                    }
                }
            } else {
                conn.connectionInfo.username = encryptedUsername;
                conn.connectionInfo.password = encryptedPassword;
            }
        } catch (...) {
            conn.connectionInfo.username = "";
            conn.connectionInfo.password = "";
        }

        std::string pathStr = columnText(stmt, 8);
        conn.connectionInfo.path = (pathStr == "NULL") ? "" : pathStr;

        conn.lastUsed = columnText(stmt, 10);
        if (conn.lastUsed == "NULL")
            conn.lastUsed = "";

        std::string workspaceIdStr = columnText(stmt, 11);
        conn.workspaceId =
            (workspaceIdStr == "NULL" || workspaceIdStr.empty()) ? 1 : std::stoi(workspaceIdStr);

        std::string showAllStr = columnText(stmt, 12);
        conn.connectionInfo.showAllDatabases =
            (showAllStr != "NULL" && showAllStr != "0" && !showAllStr.empty());

        std::string sslmodeStr = columnText(stmt, 13);
        conn.connectionInfo.sslmode =
            (sslmodeStr == "NULL" || sslmodeStr.empty())
                ? (conn.connectionInfo.type == DatabaseType::ORACLE ? SslMode::Disable
                                                                    : SslMode::Prefer)
                : stringToSslMode(sslmodeStr);

        // SSH tunnel fields (columns 14-20)
        std::string sshEnabledStr = columnText(stmt, 14);
        conn.connectionInfo.ssh.enabled =
            (sshEnabledStr != "NULL" && sshEnabledStr != "0" && !sshEnabledStr.empty());

        std::string sshHostStr = columnText(stmt, 15);
        conn.connectionInfo.ssh.host = (sshHostStr == "NULL") ? "" : sshHostStr;

        std::string sshPortStr = columnText(stmt, 16);
        conn.connectionInfo.ssh.port =
            (sshPortStr == "NULL" || sshPortStr.empty()) ? 22 : std::stoi(sshPortStr);

        std::string encryptedSshUsername = columnText(stmt, 17);
        if (encryptedSshUsername == "NULL")
            encryptedSshUsername = "";

        std::string sshAuthStr = columnText(stmt, 18);
        conn.connectionInfo.ssh.authMethod =
            (sshAuthStr == "privatekey") ? SSHAuthMethod::PrivateKey : SSHAuthMethod::Password;

        std::string sshKeyPath = columnText(stmt, 19);
        conn.connectionInfo.ssh.privateKeyPath = (sshKeyPath == "NULL") ? "" : sshKeyPath;

        std::string encryptedSshPassword = columnText(stmt, 20);
        if (encryptedSshPassword == "NULL")
            encryptedSshPassword = "";

        std::string sslCACertPath = columnText(stmt, 21);
        conn.connectionInfo.sslCACertPath = (sslCACertPath == "NULL") ? "" : sslCACertPath;

        std::string colorStr = columnText(stmt, 24);
        conn.connectionInfo.color = (colorStr == "NULL") ? "" : colorStr;

        std::string envTagStr = columnText(stmt, 25);
        conn.connectionInfo.envTag = (envTagStr == "NULL") ? "" : envTagStr;

        // Decrypt SSH credentials reusing the same per-row key
        if (!saltStr.empty() && !encryptionKey.empty()) {
            if (!encryptedSshUsername.empty()) {
                try {
                    conn.connectionInfo.ssh.username =
                        CryptoUtils::decrypt(encryptedSshUsername, encryptionKey);
                } catch (...) {
                    conn.connectionInfo.ssh.username = "";
                }
            }
            if (!encryptedSshPassword.empty()) {
                try {
                    conn.connectionInfo.ssh.password =
                        CryptoUtils::decrypt(encryptedSshPassword, encryptionKey);
                } catch (...) {
                    conn.connectionInfo.ssh.password = "";
                }
            }
        } else {
            conn.connectionInfo.ssh.username = encryptedSshUsername;
            conn.connectionInfo.ssh.password = encryptedSshPassword;
        }

        relocate(conn.connectionInfo);
        return true;
    }

} // namespace

AppState::AppState() {
    const fs::path dbPath_ = AppPaths::dataDir() / "connections.db";
    dbPath = dbPath_.string();
    spdlog::debug("app state: {}", dbPath);
}

AppState::~AppState() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool AppState::initialize(const bool mirrorToICloud) {
    // Create directory if it doesn't exist
    if (const fs::path dir = fs::path(dbPath).parent_path(); !fs::exists(dir)) {
        fs::create_directories(dir);
    }

    int rc = sqlite3_open_v2(dbPath.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open app state database: " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // limit page cache to ~1MB (state DB is tiny)
    sqlite3_exec(db_, "PRAGMA cache_size = -1000;", nullptr, nullptr, nullptr);

    if (!createTables()) {
        return false;
    }
    chooseKeyStore();
    migrateCredentialKeys();
#ifdef __APPLE__
    // read the sqlite rows before the store takes over these calls
    const auto legacy = getSavedConnections();
    const auto legacyWorkspaces = getWorkspaces();
    if (openSyncedStore(mirrorToICloud)) {
        migrateToSyncedStore(legacy, legacyWorkspaces);
        storeEnsureDefaultWorkspace();
        if (const std::string previous = MasterSecret::adoptICloudKey(); !previous.empty())
            resealConnections(previous);
    }
#else
    (void)mirrorToICloud;
#endif
    return true;
}

// the master key lives in a file unless the user turned the keychain on. installs from
// before the setting keep theirs where it is: in the keystore unless a key file exists
void AppState::chooseKeyStore() const {
    std::string keychain = getSetting("use_keychain", "");
    if (keychain.empty()) {
        bool keystoreRows = false;
        if (MasterSecret::keychainSupported() && !MasterSecret::keyFileExists()) {
            sqlite3_stmt* raw = nullptr;
            if (sqlite3_prepare_v2(db_,
                                   "SELECT 1 FROM saved_connections WHERE key_version = 1 LIMIT 1",
                                   -1, &raw, nullptr) == SQLITE_OK) {
                StmtPtr stmt(raw);
                keystoreRows = sqlite3_step(stmt.get()) == SQLITE_ROW;
            }
        }
        keychain = keystoreRows ? "1" : "0";
        setSetting("use_keychain", keychain);
    }
    MasterSecret::useKeychain(keychain == "1");
}

// one-time re-encryption of legacy rows (shipped-constant key or plaintext)
// with the keystore-backed master secret. transactional: a failure rolls back
// and the install stays fully functional on the legacy key, retried next launch.
void AppState::migrateCredentialKeys() const {
    const std::string& master = MasterSecret::get();
    if (master.empty()) {
        return; // keystore unavailable, stay on legacy key
    }

    struct Row {
        int id;
        std::string user, pass, sshUser, sshPass, salt;
    };
    std::vector<Row> rows;
    {
        const std::string sql = R"(
            SELECT id, username, password, ssh_username, ssh_password, salt
            FROM saved_connections
            WHERE COALESCE(key_version, 0) = 0;
        )";
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
            return;
        }
        StmtPtr stmt(raw);
        auto text = [&](int col) {
            std::string v = columnText(stmt.get(), col);
            return v == "NULL" ? "" : v;
        };
        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            rows.push_back(
                {sqlite3_column_int(stmt.get(), 0), text(1), text(2), text(3), text(4), text(5)});
        }
    }
    if (rows.empty()) {
        return;
    }

    if (!executeSQL("BEGIN TRANSACTION;")) {
        return;
    }

    bool ok = true;
    try {
        const std::string sql = R"(
            UPDATE saved_connections
            SET username = ?, password = ?, ssh_username = ?, ssh_password = ?,
                salt = ?, key_version = 1
            WHERE id = ?;
        )";
        for (const auto& row : rows) {
            std::string user = row.user, pass = row.pass;
            std::string sshUser = row.sshUser, sshPass = row.sshPass;
            if (!row.salt.empty()) {
                // encrypted with the legacy shipped key; undecryptable fields
                // become empty, matching load behavior
                auto saltData = CryptoUtils::base64Decode(row.salt);
                std::string oldSalt(saltData.begin(), saltData.end());
                std::string oldKey = CryptoUtils::deriveKey(CREDS_SECRET, oldSalt);
                auto dec = [&](const std::string& v) {
                    try {
                        return v.empty() ? std::string() : CryptoUtils::decrypt(v, oldKey);
                    } catch (...) {
                        return std::string();
                    }
                };
                user = dec(row.user);
                pass = dec(row.pass);
                sshUser = dec(row.sshUser);
                sshPass = dec(row.sshPass);
            }
            // else: pre-encryption plaintext row, encrypt values as-is

            std::string newSalt = CryptoUtils::generateSalt();
            std::string newKey = CryptoUtils::deriveKey(master, newSalt);
            auto enc = [&](const std::string& v) {
                return v.empty() ? std::string() : CryptoUtils::encrypt(v, newKey);
            };
            std::string saltBase64 =
                CryptoUtils::base64Encode(std::vector<uint8_t>(newSalt.begin(), newSalt.end()));

            sqlite3_stmt* raw = nullptr;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
                ok = false;
                break;
            }
            StmtPtr stmt(raw);
            sqlite3_bind_text(stmt.get(), 1, enc(user).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 2, enc(pass).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 3, enc(sshUser).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 4, enc(sshPass).c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 5, saltBase64.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt.get(), 6, row.id);
            if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
                ok = false;
                break;
            }
        }
    } catch (const std::exception& e) {
        spdlog::warn("credential key migration error: {}", e.what());
        ok = false;
    }

    if (ok && executeSQL("COMMIT;")) {
        spdlog::info("migrated {} saved connection(s) to keystore-backed encryption", rows.size());
    } else {
        executeSQL("ROLLBACK;");
        spdlog::warn("credential key migration failed, keeping legacy key; will retry next launch");
    }
}

bool AppState::createTables() {
    const std::string createConnectionsTable = R"(
        CREATE TABLE IF NOT EXISTS saved_connections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            type TEXT NOT NULL,
            host TEXT,
            port INTEGER,
            database_name TEXT,
            username TEXT,
            password TEXT,
            path TEXT,
            salt TEXT,
            last_used DATETIME DEFAULT CURRENT_TIMESTAMP,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            workspace_id INTEGER DEFAULT 1,
            show_all_databases INTEGER DEFAULT 0,
            key_version INTEGER DEFAULT 0
        );
    )";

    const std::string createSettingsTable = R"(
        CREATE TABLE IF NOT EXISTS app_settings (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createWorkspacesTable = R"(
        CREATE TABLE IF NOT EXISTS workspaces (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            description TEXT,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            last_used DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createScriptsTable = R"(
        CREATE TABLE IF NOT EXISTS sql_scripts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL,
            file_path TEXT NOT NULL UNIQUE,
            connection_id INTEGER DEFAULT 0,
            database_name TEXT DEFAULT '',
            schema_name TEXT DEFAULT '',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
    )";

    const std::string createAiSessionsTable = R"(
        CREATE TABLE IF NOT EXISTS ai_sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            backend TEXT NOT NULL,
            acp_session_id TEXT DEFAULT '',
            title TEXT NOT NULL,
            transcript TEXT DEFAULT '',
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        DELETE FROM ai_sessions WHERE updated_at < datetime('now', '-90 days');
    )";

    const bool success = executeSQL(createConnectionsTable) && executeSQL(createSettingsTable) &&
                         executeSQL(createWorkspacesTable) && executeSQL(createScriptsTable) &&
                         executeSQL(createAiSessionsTable);

    auto ensureColumnExists = [this](const std::string& columnName, const std::string& alterSql,
                                     const std::string& table = "saved_connections") {
        try {
            const std::string checkSql = "SELECT COUNT(*) FROM pragma_table_info(?) WHERE name = ?";
            sqlite3_stmt* raw = nullptr;
            const int rc = sqlite3_prepare_v2(db_, checkSql.c_str(), -1, &raw, nullptr);
            if (rc != SQLITE_OK)
                return;
            const StmtPtr stmt(raw);
            sqlite3_bind_text(stmt.get(), 1, table.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 2, columnName.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                const int count = sqlite3_column_int(stmt.get(), 0);
                if (count == 0) {
                    executeSQL(alterSql);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Failed to check column existence: " << e.what() << std::endl;
        }
    };

    ensureColumnExists("workspace_id",
                       "ALTER TABLE saved_connections ADD COLUMN workspace_id INTEGER DEFAULT 1;");
    ensureColumnExists(
        "show_all_databases",
        "ALTER TABLE saved_connections ADD COLUMN show_all_databases INTEGER DEFAULT 0;");
    ensureColumnExists("sslmode",
                       "ALTER TABLE saved_connections ADD COLUMN sslmode TEXT DEFAULT 'prefer';");

    // SSH tunnel columns
    ensureColumnExists("ssh_enabled",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_enabled INTEGER DEFAULT 0;");
    ensureColumnExists("ssh_host", "ALTER TABLE saved_connections ADD COLUMN ssh_host TEXT;");
    ensureColumnExists("ssh_port",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_port INTEGER DEFAULT 22;");
    ensureColumnExists("ssh_username",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_username TEXT;");
    ensureColumnExists(
        "ssh_auth_method",
        "ALTER TABLE saved_connections ADD COLUMN ssh_auth_method TEXT DEFAULT 'password';");
    ensureColumnExists("ssh_private_key_path",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_private_key_path TEXT;");
    ensureColumnExists("ssh_password",
                       "ALTER TABLE saved_connections ADD COLUMN ssh_password TEXT;");
    ensureColumnExists("ssl_ca_cert_path",
                       "ALTER TABLE saved_connections ADD COLUMN ssl_ca_cert_path TEXT;");
    // 0 = legacy shipped-constant key, 1 = keystore-backed master secret
    ensureColumnExists("read_only",
                       "ALTER TABLE saved_connections ADD COLUMN read_only INTEGER DEFAULT 0;");
    ensureColumnExists("key_version",
                       "ALTER TABLE saved_connections ADD COLUMN key_version INTEGER DEFAULT 0;");
    // chats belong to a connection; 0 = from before chats moved under connections
    ensureColumnExists("connection_id",
                       "ALTER TABLE ai_sessions ADD COLUMN connection_id INTEGER DEFAULT 0;",
                       "ai_sessions");

    // Presentation
    ensureColumnExists("color", "ALTER TABLE saved_connections ADD COLUMN color TEXT DEFAULT '';");
    ensureColumnExists("env_tag",
                       "ALTER TABLE saved_connections ADD COLUMN env_tag TEXT DEFAULT '';");

    // Ensure default workspace exists
    if (success) {
        ensureDefaultWorkspace();
    }

    return success;
}

bool AppState::executeSQL(const std::string& sql) const {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << (errmsg ? errmsg : "unknown") << std::endl;
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

int AppState::saveConnection(const SavedConnection& connection) const {
    VIA_STORE(true, storeSaveConnection(connection));
    const std::string sql = R"(
        INSERT OR REPLACE INTO saved_connections
        (name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id,
         show_all_databases, sslmode,
         ssh_enabled, ssh_host, ssh_port, ssh_username, ssh_auth_method, ssh_private_key_path, ssh_password,
         ssl_ca_cert_path, key_version, read_only, color, env_tag)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    const auto [secret, keyVersion] = writeSecret();
    std::string encryptionKey = CryptoUtils::deriveKey(secret, salt);
    std::string encryptedUsername =
        connection.connectionInfo.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.username, encryptionKey);
    std::string encryptedPassword =
        connection.connectionInfo.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.password, encryptionKey);
    std::string encryptedSshUsername =
        connection.connectionInfo.ssh.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.username, encryptionKey);
    std::string encryptedSshPassword =
        connection.connectionInfo.ssh.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.password, encryptionKey);

    const std::string typeStr = dearsql::databaseTypeToString(connection.connectionInfo.type);

    std::string saltBase64 =
        CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()));
    int showAll = connection.connectionInfo.showAllDatabases ? 1 : 0;

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save connection: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    StmtPtr stmt(raw);

    sqlite3_bind_text(stmt.get(), 1, connection.connectionInfo.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, typeStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, connection.connectionInfo.host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, connection.connectionInfo.port);
    sqlite3_bind_text(stmt.get(), 5, connection.connectionInfo.database.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, encryptedUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 7, encryptedPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, connection.connectionInfo.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 9, saltBase64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 10, connection.workspaceId);
    sqlite3_bind_int(stmt.get(), 11, showAll);
    auto sslmodeStr = sslModeToString(connection.connectionInfo.sslmode);
    sqlite3_bind_text(stmt.get(), 12, sslmodeStr.c_str(), -1, SQLITE_TRANSIENT);

    // SSH tunnel fields (params 13-19)
    sqlite3_bind_int(stmt.get(), 13, connection.connectionInfo.ssh.enabled ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 14, connection.connectionInfo.ssh.host.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 15, connection.connectionInfo.ssh.port);
    sqlite3_bind_text(stmt.get(), 16, encryptedSshUsername.c_str(), -1, SQLITE_TRANSIENT);
    std::string sshAuthStr = connection.connectionInfo.ssh.authMethod == SSHAuthMethod::PrivateKey
                                 ? "privatekey"
                                 : "password";
    sqlite3_bind_text(stmt.get(), 17, sshAuthStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 18, connection.connectionInfo.ssh.privateKeyPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 19, encryptedSshPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 20, connection.connectionInfo.sslCACertPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 21, keyVersion);
    sqlite3_bind_int(stmt.get(), 22, connection.connectionInfo.readOnly ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 23, connection.connectionInfo.color.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 24, connection.connectionInfo.envTag.c_str(), -1,
                      SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save connection: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }

    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

bool AppState::updateConnection(const SavedConnection& connection) const {
    VIA_STORE(true, storeUpdateConnection(connection));
    const std::string sql = R"(
        UPDATE saved_connections
        SET name = ?, type = ?, host = ?, port = ?, database_name = ?,
            username = ?, password = ?, path = ?, salt = ?, last_used = CURRENT_TIMESTAMP,
            workspace_id = ?, show_all_databases = ?, sslmode = ?,
            ssh_enabled = ?, ssh_host = ?, ssh_port = ?, ssh_username = ?,
            ssh_auth_method = ?, ssh_private_key_path = ?, ssh_password = ?,
            ssl_ca_cert_path = ?, key_version = ?, read_only = ?, color = ?, env_tag = ?
        WHERE id = ?;
    )";

    // Encrypt sensitive data
    std::string salt = CryptoUtils::generateSalt();
    const auto [secret, keyVersion] = writeSecret();
    std::string encryptionKey = CryptoUtils::deriveKey(secret, salt);
    std::string encryptedUsername =
        connection.connectionInfo.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.username, encryptionKey);
    std::string encryptedPassword =
        connection.connectionInfo.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.password, encryptionKey);
    std::string encryptedSshUsername =
        connection.connectionInfo.ssh.username.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.username, encryptionKey);
    std::string encryptedSshPassword =
        connection.connectionInfo.ssh.password.empty()
            ? ""
            : CryptoUtils::encrypt(connection.connectionInfo.ssh.password, encryptionKey);

    const std::string typeStr = dearsql::databaseTypeToString(connection.connectionInfo.type);

    std::string saltBase64 =
        CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()));
    int showAll = connection.connectionInfo.showAllDatabases ? 1 : 0;

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);

    sqlite3_bind_text(stmt.get(), 1, connection.connectionInfo.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, typeStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, connection.connectionInfo.host.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, connection.connectionInfo.port);
    sqlite3_bind_text(stmt.get(), 5, connection.connectionInfo.database.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, encryptedUsername.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 7, encryptedPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 8, connection.connectionInfo.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 9, saltBase64.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 10, connection.workspaceId);
    sqlite3_bind_int(stmt.get(), 11, showAll);
    auto sslmodeStr = sslModeToString(connection.connectionInfo.sslmode);
    sqlite3_bind_text(stmt.get(), 12, sslmodeStr.c_str(), -1, SQLITE_TRANSIENT);

    // SSH tunnel fields (params 13-19)
    sqlite3_bind_int(stmt.get(), 13, connection.connectionInfo.ssh.enabled ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 14, connection.connectionInfo.ssh.host.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 15, connection.connectionInfo.ssh.port);
    sqlite3_bind_text(stmt.get(), 16, encryptedSshUsername.c_str(), -1, SQLITE_TRANSIENT);
    std::string sshAuthStr = connection.connectionInfo.ssh.authMethod == SSHAuthMethod::PrivateKey
                                 ? "privatekey"
                                 : "password";
    sqlite3_bind_text(stmt.get(), 17, sshAuthStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 18, connection.connectionInfo.ssh.privateKeyPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 19, encryptedSshPassword.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 20, connection.connectionInfo.sslCACertPath.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 21, keyVersion);
    sqlite3_bind_int(stmt.get(), 22, connection.connectionInfo.readOnly ? 1 : 0);
    sqlite3_bind_text(stmt.get(), 23, connection.connectionInfo.color.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 24, connection.connectionInfo.envTag.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 25, connection.id);

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::vector<SavedConnection> AppState::getSavedConnections() const {
    VIA_STORE(true, storedConnections(-1));
    spdlog::debug("AppState::getSavedConnections() - Loading saved connections...");
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used,
               COALESCE(workspace_id, 1) as workspace_id,
               COALESCE(show_all_databases, 0) as show_all_databases,
               COALESCE(sslmode, 'prefer') as sslmode,
               COALESCE(ssh_enabled, 0) as ssh_enabled,
               ssh_host, COALESCE(ssh_port, 22) as ssh_port,
               ssh_username, COALESCE(ssh_auth_method, 'password') as ssh_auth_method,
               ssh_private_key_path, ssh_password,
               COALESCE(ssl_ca_cert_path, '') as ssl_ca_cert_path,
               COALESCE(key_version, 0) as key_version,
               COALESCE(read_only, 0) as read_only,
               COALESCE(color, '') as color,
               COALESCE(env_tag, '') as env_tag
        FROM saved_connections
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        spdlog::error("Failed to fetch connections: {}", sqlite3_errmsg(db_));
        return connections;
    }
    StmtPtr stmt(raw);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SavedConnection conn;
        if (!parseConnectionRow(stmt.get(), conn))
            continue;

        spdlog::debug("Loaded connection: id={}, name='{}', type={}, host='{}', port={}", conn.id,
                      conn.connectionInfo.name, static_cast<int>(conn.connectionInfo.type),
                      conn.connectionInfo.host, conn.connectionInfo.port);
        connections.push_back(conn);
    }

    spdlog::debug("AppState::getSavedConnections() - Loaded {} connections", connections.size());
    return connections;
}

bool AppState::deleteConnection(const int connectionId) const {
    VIA_STORE(true, storeDeleteConnection(connectionId));
    const std::string sql = "DELETE FROM saved_connections WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to delete connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, connectionId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to delete connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::updateSavedConnectionColumn(const char* column, const std::string& value,
                                           const int connectionId) const {
    const std::string sql =
        std::string("UPDATE saved_connections SET ") + column + " = ? WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to update " << column << ": " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, connectionId);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        std::cerr << "Failed to update " << column << ": " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::renameConnection(const int connectionId, const std::string& newName) const {
    VIA_STORE(true, storeEditConnection(connectionId, "name", newName));
    return updateSavedConnectionColumn("name", newName, connectionId);
}

bool AppState::updateConnectionEnvTag(const int connectionId, const std::string& envTag) const {
    VIA_STORE(true, storeEditConnection(connectionId, "envTag", envTag));
    return updateSavedConnectionColumn("env_tag", envTag, connectionId);
}

bool AppState::updateLastUsed(const int connectionId) const {
    VIA_STORE(true, (touchLocal("connection", connectionId), true));
    const std::string sql =
        "UPDATE saved_connections SET last_used = CURRENT_TIMESTAMP WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update last used: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, connectionId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update last used: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::setSetting(const std::string& key, const std::string& value) const {
    VIA_STORE(syncedSetting(key), storeSetSetting(key, value));
    const std::string sql = R"(
        INSERT OR REPLACE INTO app_settings (key, value, updated_at)
        VALUES (?, ?, CURRENT_TIMESTAMP);
    )";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save setting: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save setting: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::string AppState::getSetting(const std::string& key, const std::string& defaultValue) const {
    VIA_STORE(syncedSetting(key), storeGetSetting(key, defaultValue));
    const std::string sql = "SELECT value FROM app_settings WHERE key = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK)
        return defaultValue;
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt.get(), 0) != SQLITE_NULL) {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
            if (text)
                return std::string(text);
        }
    }
    return defaultValue;
}

int AppState::saveWorkspace(const Workspace& workspace) const {
    VIA_STORE(true, storeSaveWorkspace(workspace));
    const std::string sql = R"(
        INSERT INTO workspaces (name, description, last_used)
        VALUES (?, ?, CURRENT_TIMESTAMP);
    )";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save workspace: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, workspace.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, workspace.description.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save workspace: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<Workspace> AppState::getWorkspaces() const {
    VIA_STORE(true, storedWorkspaces());
    std::vector<Workspace> workspaces;
    const std::string sql = R"(
        SELECT id, name, description, created_at, last_used
        FROM workspaces
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to fetch workspaces: " << sqlite3_errmsg(db_) << std::endl;
        return workspaces;
    }
    StmtPtr stmt(raw);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        Workspace workspace;
        workspace.id = sqlite3_column_int(stmt.get(), 0);

        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1));
        workspace.name = name ? std::string(name) : "";

        std::string desc = columnText(stmt.get(), 2);
        workspace.description = (desc == "NULL") ? "" : desc;

        std::string createdAt = columnText(stmt.get(), 3);
        workspace.createdAt = (createdAt == "NULL") ? "" : createdAt;

        std::string lastUsed = columnText(stmt.get(), 4);
        workspace.lastUsed = (lastUsed == "NULL") ? "" : lastUsed;

        workspaces.push_back(workspace);
    }

    return workspaces;
}

bool AppState::deleteWorkspace(const int workspaceId) const {
    VIA_STORE(true, storeDeleteWorkspace(workspaceId));
    if (workspaceId == 1)
        return false;

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION", nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to begin transaction: " << (errmsg ? errmsg : "unknown") << std::endl;
        sqlite3_free(errmsg);
        return false;
    }

    // Move connections to default workspace
    {
        const std::string sql =
            "UPDATE saved_connections SET workspace_id = 1 WHERE workspace_id = ?";
        sqlite3_stmt* raw = nullptr;
        rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        StmtPtr stmt(raw);
        sqlite3_bind_int(stmt.get(), 1, workspaceId);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
    }

    // Delete workspace
    {
        const std::string sql = "DELETE FROM workspaces WHERE id = ?";
        sqlite3_stmt* raw = nullptr;
        rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        StmtPtr stmt(raw);
        sqlite3_bind_int(stmt.get(), 1, workspaceId);
        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            std::cerr << "Failed to delete workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
    }

    sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    return true;
}

bool AppState::renameWorkspace(const int workspaceId, const std::string& name) const {
    VIA_STORE(true, storeRenameWorkspace(workspaceId, name));
    const std::string sql = "UPDATE workspaces SET name = ? WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to rename workspace: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    auto stmt = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(raw, sqlite3_finalize);
    sqlite3_bind_text(stmt.get(), 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 2, workspaceId);
    rc = sqlite3_step(stmt.get());
    return rc == SQLITE_DONE;
}

bool AppState::updateWorkspaceLastUsed(const int workspaceId) const {
    VIA_STORE(true, (touchLocal("workspace", workspaceId), true));
    const std::string sql = "UPDATE workspaces SET last_used = CURRENT_TIMESTAMP WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update workspace usage: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, workspaceId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update workspace usage: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

std::vector<SavedConnection> AppState::getConnectionsForWorkspace(const int workspaceId) const {
    VIA_STORE(true, storedConnections(workspaceId));
    std::vector<SavedConnection> connections;

    const std::string sql = R"(
        SELECT id, name, type, host, port, database_name, username, password, path, salt, last_used, workspace_id,
               show_all_databases,
               COALESCE(sslmode, 'prefer') as sslmode,
               COALESCE(ssh_enabled, 0) as ssh_enabled,
               ssh_host, COALESCE(ssh_port, 22) as ssh_port,
               ssh_username, COALESCE(ssh_auth_method, 'password') as ssh_auth_method,
               ssh_private_key_path, ssh_password,
               COALESCE(ssl_ca_cert_path, '') as ssl_ca_cert_path,
               COALESCE(key_version, 0) as key_version,
               COALESCE(read_only, 0) as read_only,
               COALESCE(color, '') as color,
               COALESCE(env_tag, '') as env_tag
        FROM saved_connections
        WHERE workspace_id = ?
        ORDER BY last_used DESC;
    )";

    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to fetch workspace connections: " << sqlite3_errmsg(db_) << std::endl;
        return connections;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, workspaceId);

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        SavedConnection conn;
        if (!parseConnectionRow(stmt.get(), conn))
            continue;
        connections.push_back(conn);
    }

    return connections;
}

bool AppState::moveConnectionToWorkspace(const int connectionId, const int workspaceId) const {
    VIA_STORE(true,
              storeEditConnection(connectionId, "workspace", uuidFor("workspace", workspaceId)));
    const std::string sql = "UPDATE saved_connections SET workspace_id = ? WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to move connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, workspaceId);
    sqlite3_bind_int(stmt.get(), 2, connectionId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to move connection: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

int AppState::saveScript(const SqlScript& script) const {
    const std::string sql = R"(
        INSERT OR REPLACE INTO sql_scripts
        (name, file_path, connection_id, database_name, schema_name, updated_at)
        VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP);
    )";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to save script: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, script.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, script.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 3, script.connectionId);
    sqlite3_bind_text(stmt.get(), 4, script.databaseName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, script.schemaName.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to save script: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

bool AppState::updateScript(const SqlScript& script) const {
    const std::string sql = R"(
        UPDATE sql_scripts
        SET name = ?, file_path = ?, connection_id = ?, database_name = ?, schema_name = ?,
            updated_at = CURRENT_TIMESTAMP
        WHERE id = ?;
    )";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to update script: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_text(stmt.get(), 1, script.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 2, script.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 3, script.connectionId);
    sqlite3_bind_text(stmt.get(), 4, script.databaseName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, script.schemaName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 6, script.id);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to update script: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

bool AppState::deleteScript(const int scriptId) const {
    const std::string sql = "DELETE FROM sql_scripts WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to delete script: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, scriptId);
    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        std::cerr << "Failed to delete script: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }
    return true;
}

namespace {
    SqlScript parseScriptRow(sqlite3_stmt* stmt) {
        SqlScript s;
        s.id = sqlite3_column_int(stmt, 0);
        s.name = columnText(stmt, 1);
        s.filePath = columnText(stmt, 2);
        s.connectionId = sqlite3_column_int(stmt, 3);
        s.databaseName = columnText(stmt, 4);
        if (s.databaseName == "NULL")
            s.databaseName = "";
        s.schemaName = columnText(stmt, 5);
        if (s.schemaName == "NULL")
            s.schemaName = "";
        s.createdAt = columnText(stmt, 6);
        if (s.createdAt == "NULL")
            s.createdAt = "";
        s.updatedAt = columnText(stmt, 7);
        if (s.updatedAt == "NULL")
            s.updatedAt = "";
        return s;
    }
} // namespace

std::vector<SqlScript> AppState::getScriptsForConnection(const int connectionId) const {
    std::vector<SqlScript> scripts;
    const std::string sql = R"(
        SELECT id, name, file_path, connection_id, database_name, schema_name, created_at, updated_at
        FROM sql_scripts WHERE connection_id = ? ORDER BY updated_at DESC;
    )";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        return scripts;
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, connectionId);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
        scripts.push_back(parseScriptRow(stmt.get()));
    return scripts;
}

bool AppState::ensureDefaultWorkspace() const {
    VIA_STORE(true, storeEnsureDefaultWorkspace());
    // Check if default workspace exists
    {
        const std::string sql = "SELECT COUNT(*) FROM workspaces WHERE id = 1";
        sqlite3_stmt* raw = nullptr;
        int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to ensure default workspace: " << sqlite3_errmsg(db_) << std::endl;
            return false;
        }
        StmtPtr stmt(raw);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt.get(), 0);
            if (count > 0)
                return true;
        }
    }

    const std::string insertSql = R"(
        INSERT INTO workspaces (id, name, description, created_at, last_used)
        VALUES (1, 'Default', 'Default workspace for all connections', CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);
    )";
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, insertSql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to ensure default workspace: " << (errmsg ? errmsg : "unknown")
                  << std::endl;
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

int AppState::saveAiSession(const int id, const int connectionId, const std::string& backend,
                            const std::string& acpSessionId, const std::string& title,
                            const std::string& transcript) const {
    const std::string sql = R"(
        INSERT INTO ai_sessions (id, connection_id, backend, acp_session_id, title, transcript,
                                 updated_at)
        VALUES (?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
        ON CONFLICT(id) DO UPDATE SET
            connection_id = IIF(connection_id = 0, excluded.connection_id, connection_id),
            acp_session_id = excluded.acp_session_id,
            title = excluded.title,
            transcript = excluded.transcript,
            updated_at = CURRENT_TIMESTAMP;
    )";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to save ai session: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    StmtPtr stmt(raw);
    if (id > 0) {
        sqlite3_bind_int(stmt.get(), 1, id);
    } else {
        sqlite3_bind_null(stmt.get(), 1);
    }
    sqlite3_bind_int(stmt.get(), 2, connectionId);
    sqlite3_bind_text(stmt.get(), 3, backend.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 4, acpSessionId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 5, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 6, transcript.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        std::cerr << "Failed to save ai session: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }
    return id > 0 ? id : static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::vector<AiSession> AppState::getAiSessions(const int connectionId, const std::string& backend,
                                               const int limit) const {
    std::vector<AiSession> sessions;
    const std::string sql =
        "SELECT id, backend, acp_session_id, title, updated_at FROM ai_sessions "
        "WHERE connection_id IN (?, 0) AND (? = '' OR backend = ?) ORDER BY updated_at DESC LIMIT "
        "?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return sessions;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, connectionId);
    sqlite3_bind_text(stmt.get(), 2, backend.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.get(), 3, backend.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt.get(), 4, limit);
    auto col = [&](int i) {
        const auto* t = sqlite3_column_text(stmt.get(), i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    };
    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        sessions.push_back({sqlite3_column_int(stmt.get(), 0), col(1), col(2), col(3), col(4)});
    }
    return sessions;
}

std::string AppState::getAiSessionTranscript(const int id) const {
    const std::string sql = "SELECT transcript FROM ai_sessions WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return "";
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, id);
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return "";
    }
    const auto* t = sqlite3_column_text(stmt.get(), 0);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
}

bool AppState::deleteAiSession(const int id) const {
    const std::string sql = "DELETE FROM ai_sessions WHERE id = ?";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) {
        return false;
    }
    StmtPtr stmt(raw);
    sqlite3_bind_int(stmt.get(), 1, id);
    return sqlite3_step(stmt.get()) == SQLITE_DONE;
}

std::string relativeAge(const std::string& stamp) {
    std::tm tm{};
    std::istringstream in(stamp);
    in >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (in.fail()) {
        return "";
    }
#ifdef _WIN32
    const std::time_t then = _mkgmtime(&tm);
#else
    const std::time_t then = timegm(&tm);
#endif
    const auto secs = static_cast<long>(std::time(nullptr) - then);
    if (secs < 60) {
        return "now";
    }
    if (secs < 3600) {
        return std::to_string(secs / 60) + "m";
    }
    if (secs < 86400) {
        return std::to_string(secs / 3600) + "h";
    }
    return std::to_string(secs / 86400) + "d";
}

// ---- apple: connections, workspaces and synced settings in core data ----
//
// records are keyed by uuid, the synced identity; sync_ids (local sqlite, never synced)
// maps each to the int id the rest of the app uses, so scripts and chats that point at
// a connection keep working. last_used stays per device too: it changes on every
// connect and would otherwise overwrite a rename made on another device

#ifdef __APPLE__

namespace {
    using nlohmann::json;
    constexpr const char* kDefaultWorkspaceUuid = "00000000-0000-0000-0000-000000000001";

    std::string fingerprintOf(const std::string& secret) {
        if (secret.empty())
            return "v0";
        const std::string k = CryptoUtils::deriveKey(secret, "dearsql-key-id");
        return CryptoUtils::base64Encode(std::vector<uint8_t>(k.begin(), k.end())).substr(0, 12);
    }

    // which master key sealed a record: another device may hold a different one
    const std::string& keyFingerprint() {
        static std::mutex mutex;
        static std::string secret, fingerprint;
        std::lock_guard lock(mutex);
        if (secret != MasterSecret::get() || fingerprint.empty()) {
            secret = MasterSecret::get();
            fingerprint = fingerprintOf(secret);
        }
        return fingerprint;
    }

    json sealCredentials(const DatabaseConnectionInfo& info) {
        const auto [secret, keyVersion] = writeSecret();
        const std::string salt = CryptoUtils::generateSalt();
        const std::string key = CryptoUtils::deriveKey(secret, salt);
        auto enc = [&](const std::string& v) {
            return v.empty() ? std::string() : CryptoUtils::encrypt(v, key);
        };
        return {{"user", enc(info.username)},
                {"pass", enc(info.password)},
                {"sshUser", enc(info.ssh.username)},
                {"sshPass", enc(info.ssh.password)},
                {"salt", CryptoUtils::base64Encode(std::vector<uint8_t>(salt.begin(), salt.end()))},
                {"keyVersion", keyVersion},
                {"keyId", keyVersion == 1 ? keyFingerprint() : std::string("v0")}};
    }

    bool canUnseal(const json& creds) {
        return creds.value("keyVersion", 0) == 0 || creds.value("keyId", "") == keyFingerprint();
    }

    void unsealCredentials(const json& creds, DatabaseConnectionInfo& info,
                           const std::string& secret) {
        const std::string saltB64 = creds.value("salt", "");
        if (saltB64.empty())
            return;
        try {
            const auto saltData = CryptoUtils::base64Decode(saltB64);
            const std::string key =
                CryptoUtils::deriveKey(secret, std::string(saltData.begin(), saltData.end()));
            auto dec = [&](const char* field) {
                const std::string v = creds.value(field, "");
                try {
                    return v.empty() ? std::string() : CryptoUtils::decrypt(v, key);
                } catch (...) {
                    return std::string();
                }
            };
            info.username = dec("user");
            info.password = dec("pass");
            info.ssh.username = dec("sshUser");
            info.ssh.password = dec("sshPass");
        } catch (...) {
        }
    }

    bool hasCredentials(const DatabaseConnectionInfo& i) {
        return !i.username.empty() || !i.password.empty() || !i.ssh.username.empty() ||
               !i.ssh.password.empty();
    }

    // a path means nothing on another device (csv files open on duckdb)
    bool isFileType(DatabaseType t) {
        return t == DatabaseType::SQLITE || t == DatabaseType::DUCKDB;
    }

    std::string now() {
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&t, &tm);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

    json parseJson(const std::string& s) {
        json j = json::parse(s, nullptr, false);
        return j.is_object() ? j : json::object();
    }

    // one text value from a single-row query with text parameters
    std::string scalar(sqlite3* db, const char* sql, std::initializer_list<std::string> args) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
            return "";
        StmtPtr stmt(raw);
        int i = 1;
        for (const auto& a : args)
            sqlite3_bind_text(stmt.get(), i++, a.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) != SQLITE_ROW ||
            sqlite3_column_type(stmt.get(), 0) == SQLITE_NULL)
            return "";
        const auto* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        return t ? t : "";
    }
} // namespace

bool AppState::syncedSetting(const std::string& key) {
    // the rest stay on the device: window and display, keys and licence activation,
    // agent sign-in, the current workspace
    return key == "theme" || key == "ai_provider" || key == "ai_sidebar_backend" ||
           key == "ai_mcp_enabled" || key.starts_with("acp_model_");
}

bool AppState::openSyncedStore(const bool mirror) {
    executeSQL("CREATE TABLE IF NOT EXISTS sync_ids (kind TEXT NOT NULL, uuid TEXT NOT NULL, "
               "local_id INTEGER NOT NULL, last_used TEXT DEFAULT '', PRIMARY KEY (kind, uuid));");
    deviceId_ = getSetting("device_id");
    if (deviceId_.empty()) {
        deviceId_ = SyncedStore::newUuid();
        setSetting("device_id", deviceId_);
    }
    const fs::path file = fs::path(dbPath).parent_path() / "synced.sqlite";
    auto store = std::make_unique<SyncedStore>(file.string(), mirror);
    if (!store->ok()) {
        spdlog::error("synced store unavailable, keeping connections in {}", dbPath);
        return false;
    }
    store_ = std::move(store);
    return true;
}

void AppState::mapLocal(const std::string& kind, const std::string& uuid, const int localId,
                        const std::string& lastUsed) const {
    scalar(db_,
           "INSERT OR REPLACE INTO sync_ids (kind, uuid, local_id, last_used) VALUES (?, ?, ?, ?)",
           {kind, uuid, std::to_string(localId), lastUsed});
}

int AppState::localId(const std::string& kind, const std::string& uuid) const {
    if (kind == "workspace" && uuid == kDefaultWorkspaceUuid)
        return 1;
    static std::mutex mutex; // two new uuids must not get the same id
    std::lock_guard lock(mutex);
    if (const std::string id =
            scalar(db_, "SELECT local_id FROM sync_ids WHERE kind = ? AND uuid = ?", {kind, uuid});
        !id.empty())
        return std::stoi(id);
    // past every id this install has used, synced or from before the store
    const char* legacy = kind == "connection" ? "SELECT MAX(id) FROM saved_connections"
                                              : "SELECT MAX(id) FROM workspaces";
    const std::string maxMapped =
        scalar(db_, "SELECT MAX(local_id) FROM sync_ids WHERE kind = ?", {kind});
    const std::string maxLegacy = scalar(db_, legacy, {});
    const int next = std::max({maxMapped.empty() ? 0 : std::stoi(maxMapped),
                               maxLegacy.empty() ? 0 : std::stoi(maxLegacy), 1}) +
                     1;
    mapLocal(kind, uuid, next, "");
    return next;
}

std::string AppState::uuidFor(const std::string& kind, const int localId) const {
    if (kind == "workspace" && localId == 1)
        return kDefaultWorkspaceUuid;
    return scalar(db_, "SELECT uuid FROM sync_ids WHERE kind = ? AND local_id = ?",
                  {kind, std::to_string(localId)});
}

void AppState::forgetLocal(const std::string& kind, const int localId) const {
    scalar(db_, "DELETE FROM sync_ids WHERE kind = ? AND local_id = ?",
           {kind, std::to_string(localId)});
}

std::string AppState::localLastUsed(const std::string& kind, const int localId) const {
    return scalar(db_, "SELECT last_used FROM sync_ids WHERE kind = ? AND local_id = ?",
                  {kind, std::to_string(localId)});
}

void AppState::touchLocal(const std::string& kind, const int localId) const {
    const std::string uuid = uuidFor(kind, localId);
    if (!uuid.empty())
        mapLocal(kind, uuid, localId, now());
}

std::string AppState::storedConnectionJson(const SavedConnection& c,
                                           const std::string& workspaceUuid) const {
    const auto& i = c.connectionInfo;
    json j = {{"name", i.name},
              {"type", dearsql::databaseTypeToString(i.type)},
              {"host", i.host},
              {"port", i.port},
              {"database", i.database},
              {"path", i.path},
              {"workspace", workspaceUuid},
              {"showAllDatabases", i.showAllDatabases},
              {"sslmode", sslModeToString(i.sslmode)},
              {"sslCACertPath", i.sslCACertPath},
              {"readOnly", i.readOnly},
              {"color", i.color},
              {"envTag", i.envTag},
              {"ssh",
               {{"enabled", i.ssh.enabled},
                {"host", i.ssh.host},
                {"port", i.ssh.port},
                {"auth", i.ssh.authMethod == SSHAuthMethod::PrivateKey ? "privatekey" : "password"},
                {"keyPath", i.ssh.privateKeyPath}}}};
    return j.dump();
}

std::vector<SavedConnection> AppState::storedConnections(const int workspaceId) const {
    std::vector<std::string> workspaceUuids;
    for (const auto& w : store_->all("workspace"))
        workspaceUuids.push_back(w.uuid);
    std::vector<SavedConnection> out;
    for (const auto& r : store_->all("connection")) {
        const json j = parseJson(r.json);
        SavedConnection c;
        auto& i = c.connectionInfo;
        const std::string type = j.value("type", "");
        i.type = dearsql::stringToDatabaseType(type);
        if (dearsql::databaseTypeToString(i.type) != type)
            continue;
        if (isFileType(i.type) && j.value("device", "") != deviceId_)
            continue; // the file is on another device
        std::string workspace = j.value("workspace", kDefaultWorkspaceUuid);
        if (std::ranges::find(workspaceUuids, workspace) == workspaceUuids.end())
            workspace = kDefaultWorkspaceUuid; // deleted on another device
        c.workspaceId = localId("workspace", workspace);
        if (workspaceId >= 0 && c.workspaceId != workspaceId)
            continue;
        i.name = j.value("name", "");
        i.host = j.value("host", "");
        i.port = j.value("port", 0);
        i.database = j.value("database", "");
        i.path = j.value("path", "");
        i.showAllDatabases = j.value("showAllDatabases", false);
        const std::string sslmode = j.value("sslmode", "");
        i.sslmode = sslmode.empty()
                        ? (i.type == DatabaseType::ORACLE ? SslMode::Disable : SslMode::Prefer)
                        : stringToSslMode(sslmode);
        i.sslCACertPath = j.value("sslCACertPath", "");
        i.readOnly = j.value("readOnly", false);
        i.color = j.value("color", "");
        i.envTag = j.value("envTag", "");
        const json ssh = j.value("ssh", json::object());
        i.ssh.enabled = ssh.value("enabled", false);
        i.ssh.host = ssh.value("host", "");
        i.ssh.port = ssh.value("port", 22);
        i.ssh.authMethod = ssh.value("auth", "") == "privatekey" ? SSHAuthMethod::PrivateKey
                                                                 : SSHAuthMethod::Password;
        i.ssh.privateKeyPath = ssh.value("keyPath", "");
        const json creds = j.value("creds", json::object());
        if (canUnseal(creds))
            unsealCredentials(creds, i, baseSecretForVersion(creds.value("keyVersion", 0)));
        c.id = localId("connection", r.uuid);
        c.lastUsed = localLastUsed("connection", c.id);
        relocate(c.connectionInfo);
        out.push_back(std::move(c));
    }
    std::ranges::stable_sort(out, std::greater{}, &SavedConnection::lastUsed);
    return out;
}

int AppState::storeSaveConnection(const SavedConnection& c) const {
    const std::string uuid = SyncedStore::newUuid();
    std::string workspace = uuidFor("workspace", c.workspaceId);
    json j =
        parseJson(storedConnectionJson(c, workspace.empty() ? kDefaultWorkspaceUuid : workspace));
    j["creds"] = sealCredentials(c.connectionInfo);
    j["device"] = deviceId_;
    j["created"] = now();
    store_->put("connection", uuid, j.dump());
    const int id = localId("connection", uuid);
    touchLocal("connection", id);
    return id;
}

bool AppState::storeUpdateConnection(const SavedConnection& c) const {
    const std::string uuid = uuidFor("connection", c.id);
    if (uuid.empty())
        return false;
    json old = json::object();
    for (const auto& r : store_->all("connection"))
        if (r.uuid == uuid)
            old = parseJson(r.json);
    const std::string workspace = uuidFor("workspace", c.workspaceId);
    json j =
        parseJson(storedConnectionJson(c, workspace.empty() ? kDefaultWorkspaceUuid : workspace));
    // credentials this device can't read come back empty from the edit dialog: saving
    // them would wipe the good ones on the device that wrote them
    const json oldCreds = old.value("creds", json::object());
    j["creds"] = !canUnseal(oldCreds) && !hasCredentials(c.connectionInfo)
                     ? oldCreds
                     : sealCredentials(c.connectionInfo);
    j["device"] = old.value("device", deviceId_);
    j["created"] = old.value("created", now());
    store_->put("connection", uuid, j.dump());
    touchLocal("connection", c.id);
    return true;
}

bool AppState::storeEditConnection(const int id, const char* field,
                                   const std::string& value) const {
    const std::string uuid = uuidFor("connection", id);
    for (const auto& r : store_->all("connection")) {
        if (r.uuid != uuid)
            continue;
        json j = parseJson(r.json);
        j[field] = value;
        store_->put("connection", uuid, j.dump());
        return true;
    }
    return false;
}

bool AppState::storeDeleteConnection(const int id) const {
    const std::string uuid = uuidFor("connection", id);
    if (uuid.empty())
        return false;
    store_->remove("connection", uuid);
    forgetLocal("connection", id);
    return true;
}

std::vector<Workspace> AppState::storedWorkspaces() const {
    std::vector<Workspace> out;
    for (const auto& r : store_->all("workspace")) {
        const json j = parseJson(r.json);
        Workspace w;
        w.id = localId("workspace", r.uuid);
        w.name = j.value("name", "");
        w.description = j.value("description", "");
        w.createdAt = j.value("created", "");
        w.lastUsed = localLastUsed("workspace", w.id);
        out.push_back(std::move(w));
    }
    std::ranges::stable_sort(out, std::greater{}, &Workspace::lastUsed);
    return out;
}

int AppState::storeSaveWorkspace(const Workspace& w) const {
    for (const auto& existing : storedWorkspaces())
        if (existing.name == w.name)
            return -1; // names are unique, as in the sqlite table
    const std::string uuid = SyncedStore::newUuid();
    store_->put("workspace", uuid,
                json{{"name", w.name}, {"description", w.description}, {"created", now()}}.dump());
    const int id = localId("workspace", uuid);
    touchLocal("workspace", id);
    return id;
}

bool AppState::storeDeleteWorkspace(const int id) const {
    const std::string uuid = uuidFor("workspace", id);
    if (id == 1 || uuid.empty())
        return false;
    for (const auto& r : store_->all("connection")) {
        json j = parseJson(r.json);
        if (j.value("workspace", "") == uuid) {
            j["workspace"] = kDefaultWorkspaceUuid;
            store_->put("connection", r.uuid, j.dump());
        }
    }
    store_->remove("workspace", uuid);
    forgetLocal("workspace", id);
    return true;
}

bool AppState::storeRenameWorkspace(const int id, const std::string& name) const {
    const std::string uuid = uuidFor("workspace", id);
    for (const auto& r : store_->all("workspace")) {
        if (r.uuid != uuid)
            continue;
        json j = parseJson(r.json);
        j["name"] = name;
        store_->put("workspace", uuid, j.dump());
        return true;
    }
    return false;
}

bool AppState::storeEnsureDefaultWorkspace() const {
    for (const auto& r : store_->all("workspace"))
        if (r.uuid == kDefaultWorkspaceUuid)
            return true;
    store_->put("workspace", kDefaultWorkspaceUuid,
                json{{"name", "Default"},
                     {"description", "Default workspace for all connections"},
                     {"created", now()}}
                    .dump());
    return true;
}

bool AppState::storeSetSetting(const std::string& key, const std::string& value) const {
    store_->put("setting", key, json{{"value", value}}.dump());
    std::lock_guard lock(settingsMutex_);
    settingsCache_[key] = value;
    return true;
}

std::string AppState::storeGetSetting(const std::string& key,
                                      const std::string& defaultValue) const {
    std::lock_guard lock(settingsMutex_);
    if (!settingsLoaded_) {
        settingsCache_.clear();
        for (const auto& r : store_->all("setting"))
            settingsCache_[r.uuid] = parseJson(r.json).value("value", "");
        settingsLoaded_ = true;
    }
    const auto it = settingsCache_.find(key);
    return it == settingsCache_.end() ? defaultValue : it->second;
}

// once per install: copy the sqlite rows into the store. the old tables stay as they
// were, and a copy of the whole file is kept, so nothing here can lose data
void AppState::migrateToSyncedStore(const std::vector<SavedConnection>& legacy,
                                    const std::vector<Workspace>& legacyWorkspaces) {
    if (getSetting("synced_store_migrated") == "1")
        return;
    const fs::path backup = fs::path(dbPath).parent_path() / "connections-before-sync.db";
    if (std::error_code ec; !fs::exists(backup, ec))
        scalar(db_, "VACUUM INTO ?", {backup.string()});

    storeEnsureDefaultWorkspace();
    std::vector<Workspace> synced = storedWorkspaces();
    for (const auto& w : legacyWorkspaces) {
        if (w.id == 1)
            continue;
        // the same workspace migrated from another device already: share it
        const auto same = std::ranges::find(synced, w.name, &Workspace::name);
        std::string uuid = same != synced.end() ? uuidFor("workspace", same->id) : "";
        if (uuid.empty()) {
            uuid = SyncedStore::newUuid();
            store_->put(
                "workspace", uuid,
                json{{"name", w.name}, {"description", w.description}, {"created", w.createdAt}}
                    .dump());
        }
        if (same != synced.end())
            forgetLocal("workspace", same->id);
        mapLocal("workspace", uuid, w.id, w.lastUsed);
    }
    for (const auto& c : legacy) {
        const std::string uuid = SyncedStore::newUuid();
        const std::string workspace = uuidFor("workspace", c.workspaceId);
        json j = parseJson(
            storedConnectionJson(c, workspace.empty() ? kDefaultWorkspaceUuid : workspace));
        j["creds"] = sealCredentials(c.connectionInfo);
        j["device"] = deviceId_;
        j["created"] = c.lastUsed;
        store_->put("connection", uuid, j.dump());
        mapLocal("connection", uuid, c.id, c.lastUsed); // scripts and chats keep their link
    }
    {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT key, value FROM app_settings", -1, &raw, nullptr) ==
            SQLITE_OK) {
            StmtPtr stmt(raw);
            while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
                const std::string key = columnText(stmt.get(), 0);
                if (syncedSetting(key) && storeGetSetting(key, "\x01") == "\x01")
                    storeSetSetting(key, columnText(stmt.get(), 1));
            }
        }
    }
    setSetting("synced_store_migrated", "1");
    spdlog::info("moved {} connection(s) and {} workspace(s) to the synced store (backup: {})",
                 legacy.size(), legacyWorkspaces.size(), backup.string());
}

// the master key changed to the one icloud keychain shares: records this device sealed
// with its old key are opened with that and sealed again
void AppState::resealConnections(const std::string& previousSecret) const {
    const std::string previousId = fingerprintOf(previousSecret);
    for (const auto& r : store_->all("connection")) {
        json j = parseJson(r.json);
        const json creds = j.value("creds", json::object());
        if (creds.value("keyVersion", 0) != 1 || creds.value("keyId", "") != previousId)
            continue;
        DatabaseConnectionInfo info;
        unsealCredentials(creds, info, previousSecret);
        j["creds"] = sealCredentials(info);
        store_->put("connection", r.uuid, j.dump());
    }
}
#endif

bool AppState::takeRemoteChange() const {
#ifdef __APPLE__
    if (store_ && store_->takeRemoteChange()) {
        std::lock_guard lock(settingsMutex_);
        settingsLoaded_ = false;
        return true;
    }
#endif
    return false;
}
