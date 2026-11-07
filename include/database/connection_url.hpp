#pragma once

#include "db_interface.hpp"
#include <string>

// Parses a database connection URL into a DatabaseConnectionInfo.
//
// Supported schemes (case-insensitive):
//   sqlite://[/]<path>              -> SQLITE
//   postgresql:// | postgres://     -> POSTGRESQL
//   redshift://                     -> REDSHIFT
//   mysql://                        -> MYSQL
//   mariadb://                      -> MARIADB
//   tidb://                         -> TIDB
//   cockroachdb:// | cockroach://   -> COCKROACHDB
//   mongodb:// | mongodb+srv://     -> MONGODB
//   redis://                        -> REDIS
//   rediss://                       -> REDIS with TLS (sslmode=require)
//   mssql:// | sqlserver://         -> MSSQL
//   oracle://                       -> ORACLE
//
// Format for server URLs:
//   scheme://[user[:password]@]host[:port][/database][?key=value&...]
//
// Recognised query parameters:
//   sslmode=disable|allow|prefer|require|verify-ca|verify-full|verify-identity
//   sslrootcert=/path/to/ca.pem  (alias: tlsCAFile)
//   tls=true|false               (mongodb-style; true => require)
//
// User/password components are percent-decoded.
struct ConnectionUrlParseResult {
    bool ok = false;
    std::string error;
    DatabaseConnectionInfo info;
};

ConnectionUrlParseResult parseConnectionUrl(const std::string& url);

// Cheap pre-check: does the string look like a connection URL the parser
// recognises? Used by UI to auto-detect pasted URLs.
bool looksLikeConnectionUrl(const std::string& s);

// Render a DatabaseConnectionInfo as a connection URL. Inverse of
// parseConnectionUrl for the fields it exposes — round-trips via
// parseConnectionUrl(buildConnectionUrl(info)) for typical inputs.
//
// Notes:
//   - SSH config is NOT representable in a URL and is dropped.
//   - "show all databases" is NOT in the URL.
//   - Port is always included when > 0 so the URL stays in sync with the
//     Port field in the connection dialog.
//   - Username/password/database are percent-encoded.
//   - Returns "" for types with no defined URL form (e.g. CASSANDRA).
std::string buildConnectionUrl(const DatabaseConnectionInfo& info);
