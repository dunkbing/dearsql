#pragma once

#include "db_interface.hpp"

struct SslModeConfig {
    const char* const* labels;
    const SslMode* values;
    int count;
    int defaultIdx;
};

// PostgreSQL: full 6-mode sslmode
inline constexpr const char* kPgSslLabels[] = {"disable", "allow",     "prefer",
                                               "require", "verify-ca", "verify-full"};
inline constexpr SslMode kPgSslValues[] = {SslMode::Disable,  SslMode::Allow,
                                           SslMode::Prefer,   SslMode::Require,
                                           SslMode::VerifyCA, SslMode::VerifyFull};

// MySQL: no "allow" (maps to same as "prefer")
inline constexpr const char* kMySqlSslLabels[] = {"disable", "prefer", "require", "verify-ca",
                                                  "verify-identity"};
inline constexpr SslMode kMySqlSslValues[] = {SslMode::Disable, SslMode::Prefer, SslMode::Require,
                                              SslMode::VerifyCA, SslMode::VerifyIdentity};

// MSSQL: Encrypt attribute modes
inline constexpr const char* kMssqlSslLabels[] = {"Off", "Encrypt", "Encrypt + Verify", "Strict"};
inline constexpr SslMode kMssqlSslValues[] = {SslMode::Disable, SslMode::Require, SslMode::VerifyCA,
                                              SslMode::VerifyFull};

// Oracle: wallet-based TLS (simplified)
inline constexpr const char* kOracleSslLabels[] = {"Off", "TLS", "TLS + Verify"};
inline constexpr SslMode kOracleSslValues[] = {SslMode::Disable, SslMode::Require,
                                               SslMode::VerifyCA};

// MongoDB/Redis: simple on/off TLS
inline constexpr const char* kSimpleSslLabels[] = {"Off", "TLS", "TLS + Verify CA"};
inline constexpr SslMode kSimpleSslModeValues[] = {SslMode::Disable, SslMode::Require,
                                                   SslMode::VerifyCA};

inline SslModeConfig getSslConfig(DatabaseType type) {
    switch (type) {
    case DatabaseType::REDSHIFT:
    case DatabaseType::POSTGRESQL:
        return {kPgSslLabels, kPgSslValues, 6, 2};
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        return {kMySqlSslLabels, kMySqlSslValues, 5, 1};
    case DatabaseType::MSSQL:
        return {kMssqlSslLabels, kMssqlSslValues, 4, 0};
    case DatabaseType::ORACLE:
        return {kOracleSslLabels, kOracleSslValues, 3, 0};
    default:
        return {kSimpleSslLabels, kSimpleSslModeValues, 3, 0};
    }
}

inline bool sslModeNeedsCACert(SslMode mode) {
    return mode == SslMode::VerifyCA || mode == SslMode::VerifyFull ||
           mode == SslMode::VerifyIdentity;
}
