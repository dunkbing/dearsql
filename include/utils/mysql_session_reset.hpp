#pragma once

#include <mysql.h>
#include <spdlog/spdlog.h>
#include <string>
#include <utility>

/**
 * @brief Returns a pooled MySQL connection to the state the pool handed it out in.
 *
 * A dump applies to the session, not to a statement: LOCK TABLES holds until an
 * UNLOCK, and the preamble leaves FOREIGN_KEY_CHECKS and SQL_MODE changed. Stopping
 * partway -- on cancel, or on a failed statement -- would otherwise return a
 * connection that still holds a write lock, blocking every later reader of that
 * table, and that silently skips referential integrity for whatever runs next.
 *
 * Declare after the Session so this runs before the release.
 */
class MySQLSessionReset {
public:
    MySQLSessionReset(MYSQL* conn, std::string database)
        : conn_(conn), database_(std::move(database)) {}

    ~MySQLSessionReset() {
        if (conn_ == nullptr) {
            return;
        }
        if (mysql_reset_connection(conn_) != 0) {
            spdlog::warn("Could not reset the pooled session: {}", mysql_error(conn_));
            return;
        }
        // A reset keeps the default database in practice, but that is not documented
        // and the pool hands these connections out per database.
        if (mysql_select_db(conn_, database_.c_str()) != 0) {
            spdlog::warn("Could not reselect '{}' after resetting the session: {}", database_,
                         mysql_error(conn_));
        }
        // The reset returns the server to character_set_server and does not update
        // what the client library believes it negotiated, so the two diverge on any
        // server whose default is not utf8mb4. mysql_real_escape_string escapes
        // according to the client's belief, so leaving them apart corrupts writes.
        // Must match the charset the connection factory applies.
        if (mysql_set_character_set(conn_, "utf8mb4") != 0) {
            spdlog::warn("Could not restore the session character set: {}", mysql_error(conn_));
        }
    }

    MySQLSessionReset(const MySQLSessionReset&) = delete;
    MySQLSessionReset& operator=(const MySQLSessionReset&) = delete;
    MySQLSessionReset(MySQLSessionReset&&) = delete;
    MySQLSessionReset& operator=(MySQLSessionReset&&) = delete;

private:
    MYSQL* conn_;
    std::string database_;
};
