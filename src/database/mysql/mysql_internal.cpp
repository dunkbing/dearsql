#include "database/mysql/mysql_internal.hpp"
#include <chrono>
#include <format>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace mysql_internal {

    // setting MYSQL_OPT_SSL_CA implicitly enables CA verification, so only apply
    // it for the verify-* modes. Require encrypts but does not verify.
    bool shouldApplySslCA(const DatabaseConnectionInfo& info) {
        if (info.sslCACertPath.empty()) {
            return false;
        }

        switch (info.sslmode) {
        case SslMode::VerifyCA:
        case SslMode::VerifyFull:
        case SslMode::VerifyIdentity:
            return true;
        default:
            return false;
        }
    }

    std::function<MYSQL*()> makeMysqlFactory(const DatabaseConnectionInfo& info) {
        return [info]() -> MYSQL* {
            MYSQL* conn = mysql_init(nullptr);
            if (!conn) {
                throw std::runtime_error("mysql_init failed");
            }

            constexpr unsigned int connectTimeoutSeconds = 5;
            mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeoutSeconds);

            // force TCP so host="localhost" doesn't fall back to a unix socket
            constexpr unsigned int tcpProtocol = MYSQL_PROTOCOL_TCP;
            mysql_options(conn, MYSQL_OPT_PROTOCOL, &tcpProtocol);

            if (shouldApplySslCA(info)) {
                mysql_options(conn, MYSQL_OPT_SSL_CA, info.sslCACertPath.c_str());
            }

            // libmariadb verifies against the OS trust store by default once
            // SSL is negotiated, which rejects self-signed certs. Explicitly
            // disable verification for non-verify modes so connections to
            // self-signed servers (local, docker, internal CAs) succeed, and
            // explicitly enable it for verify-* modes so a user-supplied CA
            // actually gates the connection.
            //
            // libmariadb does not expose MYSQL_OPT_SSL_MODE, so VerifyCA and
            // VerifyFull/VerifyIdentity both end up performing cert + hostname
            // verification (libmariadb cannot do CA-only without hostname).
            {
                my_bool enforce = 0;
                my_bool verify = 0;
                switch (info.sslmode) {
                case SslMode::Disable:
                    enforce = 0;
                    verify = 0;
                    break;
                case SslMode::Require:
                    enforce = 1;
                    verify = 0;
                    break;
                case SslMode::VerifyCA:
                case SslMode::VerifyFull:
                case SslMode::VerifyIdentity:
                    enforce = 1;
                    verify = 1;
                    break;
                default: // Prefer / Allow: SSL if available, no verification
                    enforce = 0;
                    verify = 0;
                    break;
                }
                mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enforce);
                mysql_options(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
            }

            unsigned long flags = CLIENT_MULTI_STATEMENTS;
            // nullptr db = no default schema; needed for users without grants
            // on any particular database (#19)
            if (!mysql_real_connect(conn, info.host.c_str(), info.username.c_str(),
                                    info.password.c_str(),
                                    info.database.empty() ? nullptr : info.database.c_str(),
                                    info.port, nullptr, flags)) {
                std::string err = mysql_error(conn);
                mysql_close(conn);
                throw std::runtime_error("MySQL connection failed: " + err);
            }
            mysql_set_character_set(conn, "utf8mb4");
            return conn;
        };
    }

    StatementResult extractMysqlResult(MYSQL* conn, int rowLimit, double* downloadMs,
                                       double* parseMs) {
        StatementResult result;
        using Clock = std::chrono::high_resolution_clock;
        const auto toMs = [](auto d) {
            return std::chrono::duration<double, std::milli>(d).count();
        };

        const auto tDownload = Clock::now();
        MYSQL_RES* rawRes = mysql_store_result(conn);
        const auto tParse = Clock::now();
        if (downloadMs) {
            *downloadMs += toMs(tParse - tDownload);
        }
        if (rawRes) {
            MysqlResPtr res(rawRes);
            unsigned int nFields = mysql_num_fields(res.get());
            MYSQL_FIELD* fields = mysql_fetch_fields(res.get());

            std::vector<bool> isBoolCol(nFields, false);
            for (unsigned int i = 0; i < nFields; i++) {
                result.columnNames.emplace_back(fields[i].name);
                isBoolCol[i] = (fields[i].type == MYSQL_TYPE_TINY && fields[i].length == 1);
            }

            int rowCount = 0;
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr && rowCount < rowLimit) {
                unsigned long* lengths = mysql_fetch_lengths(res.get());
                std::vector<std::string> rowData;
                rowData.reserve(nFields);
                for (unsigned int i = 0; i < nFields; i++) {
                    if (row[i] == nullptr) {
                        rowData.emplace_back("NULL");
                    } else if (isBoolCol[i]) {
                        rowData.emplace_back(row[i][0] == '1' ? BOOL_TRUE_SENTINEL
                                                              : BOOL_FALSE_SENTINEL);
                    } else {
                        rowData.emplace_back(row[i], lengths[i]);
                    }
                }
                result.tableData.push_back(std::move(rowData));
                rowCount++;
            }

            result.message = std::format("Returned {} row{}", result.tableData.size(),
                                         result.tableData.size() == 1 ? "" : "s");
            my_ulonglong totalRows = mysql_num_rows(res.get());
            if (static_cast<int>(totalRows) >= rowLimit) {
                result.message += std::format(" (limited to {})", rowLimit);
            }
        } else {
            if (mysql_field_count(conn) == 0) {
                my_ulonglong affected = mysql_affected_rows(conn);
                if (affected != (my_ulonglong)-1) {
                    result.message = std::format("{} row(s) affected", affected);
                } else {
                    result.message = "Query executed successfully";
                }
            } else {
                result.success = false;
                result.errorMessage = mysql_error(conn);
            }
        }

        if (parseMs) {
            *parseMs += toMs(Clock::now() - tParse);
        }
        return result;
    }

    std::function<void(MYSQL*)> makeMysqlCanceller(const DatabaseConnectionInfo& info) {
        // a busy MYSQL* cannot issue anything itself, so kill from a throwaway connection
        return [factory = makeMysqlFactory(info)](MYSQL* conn) {
            try {
                MYSQL* killer = factory();
                const std::string sql = std::format("KILL QUERY {}", mysql_thread_id(conn));
                if (mysql_query(killer, sql.c_str()) != 0) {
                    spdlog::warn("{} failed: {}", sql, mysql_error(killer));
                }
                mysql_close(killer);
            } catch (const std::exception& e) {
                spdlog::warn("could not open a connection to cancel the query: {}", e.what());
            }
        };
    }

} // namespace mysql_internal
