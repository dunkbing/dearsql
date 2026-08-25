#include "database/connection_url.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

    std::string toLower(std::string_view s) {
        std::string out(s);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    // percent-decode src; returns false on malformed escapes.
    bool percentDecode(std::string_view src, std::string& out) {
        out.clear();
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            char c = src[i];
            if (c == '%') {
                if (i + 2 >= src.size())
                    return false;
                auto hex = [](char ch, int& v) {
                    if (ch >= '0' && ch <= '9') {
                        v = ch - '0';
                        return true;
                    }
                    if (ch >= 'a' && ch <= 'f') {
                        v = 10 + (ch - 'a');
                        return true;
                    }
                    if (ch >= 'A' && ch <= 'F') {
                        v = 10 + (ch - 'A');
                        return true;
                    }
                    return false;
                };
                int hi = 0, lo = 0;
                if (!hex(src[i + 1], hi) || !hex(src[i + 2], lo))
                    return false;
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else if (c == '+') {
                // intentional: keep '+' literal — passwords commonly contain it,
                // and database URLs don't follow form-encoding rules.
                out.push_back('+');
            } else {
                out.push_back(c);
            }
        }
        return true;
    }

    struct SchemeInfo {
        DatabaseType type;
        int defaultPort;
        bool tls;
    };

    std::optional<SchemeInfo> resolveScheme(std::string_view scheme) {
        static const std::unordered_map<std::string, SchemeInfo> kSchemes = {
            {"sqlite", {DatabaseType::SQLITE, 0, false}},
            {"duckdb", {DatabaseType::DUCKDB, 0, false}},
            {"postgresql", {DatabaseType::POSTGRESQL, 5432, false}},
            {"postgres", {DatabaseType::POSTGRESQL, 5432, false}},
            {"redshift", {DatabaseType::REDSHIFT, 5439, false}},
            {"mysql", {DatabaseType::MYSQL, 3306, false}},
            {"mariadb", {DatabaseType::MARIADB, 3306, false}},
            {"mongodb", {DatabaseType::MONGODB, 27017, false}},
            {"mongodb+srv", {DatabaseType::MONGODB, 27017, false}},
            {"redis", {DatabaseType::REDIS, 6379, false}},
            {"rediss", {DatabaseType::REDIS, 6379, true}},
            {"mssql", {DatabaseType::MSSQL, 1433, false}},
            {"sqlserver", {DatabaseType::MSSQL, 1433, false}},
            {"oracle", {DatabaseType::ORACLE, 1521, false}},
        };
        auto it = kSchemes.find(toLower(scheme));
        if (it == kSchemes.end())
            return std::nullopt;
        return it->second;
    }

    void applySslmodeFromQuery(DatabaseConnectionInfo& info, const std::string& key,
                               const std::string& value) {
        if (key == "sslmode" || key == "ssl_mode") {
            info.sslmode = stringToSslMode(value);
        } else if (key == "sslrootcert" || key == "tlscafile" || key == "ssl_ca" ||
                   key == "sslca") {
            info.sslCACertPath = value;
        } else if (key == "tls" || key == "ssl") {
            std::string v = toLower(value);
            if (v == "true" || v == "1" || v == "yes" || v == "required") {
                if (info.sslmode == SslMode::Prefer || info.sslmode == SslMode::Disable ||
                    info.sslmode == SslMode::Allow) {
                    info.sslmode = SslMode::Require;
                }
            } else if (v == "false" || v == "0" || v == "no") {
                info.sslmode = SslMode::Disable;
            }
        }
    }

    bool parsePort(std::string_view portStr, int& port) {
        if (portStr.empty())
            return false;
        int parsed = 0;
        const char* begin = portStr.data();
        const char* end = begin + portStr.size();
        auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc{} || ptr != end)
            return false;
        port = parsed;
        return true;
    }

} // namespace

namespace {

    // RFC 3986 percent-encode: keep unreserved characters as-is, escape everything
    // else. Conservative — we don't preserve sub-delims for readability.
    std::string percentEncode(std::string_view s) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(static_cast<char>(c));
            } else {
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0xF]);
            }
        }
        return out;
    }

} // namespace

std::string buildConnectionUrl(const DatabaseConnectionInfo& info) {
    if (isFileDatabase(info.type)) {
        // sqlite:///<path> — keep '/' and ':' literal so the URL is human-
        // readable; only encode characters that would break URL parsing
        // (space, '?', '#', '%', etc.).
        std::string out = (info.type == DatabaseType::DUCKDB) ? "duckdb:///" : "sqlite:///";
        for (unsigned char c : info.path) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':' || c == '\\') {
                out.push_back(static_cast<char>(c));
            } else {
                static constexpr char kHex[] = "0123456789ABCDEF";
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0xF]);
            }
        }
        return out;
    }

    const char* scheme = nullptr;
    switch (info.type) {
    case DatabaseType::POSTGRESQL:
        scheme = "postgresql";
        break;
    case DatabaseType::REDSHIFT:
        scheme = "redshift";
        break;
    case DatabaseType::MYSQL:
        scheme = "mysql";
        break;
    case DatabaseType::MARIADB:
        scheme = "mariadb";
        break;
    case DatabaseType::MONGODB:
        scheme = "mongodb";
        break;
    case DatabaseType::REDIS:
        // rediss:// when TLS is on, redis:// otherwise.
        scheme = (info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
                  info.sslmode == SslMode::VerifyFull)
                     ? "rediss"
                     : "redis";
        break;
    case DatabaseType::MSSQL:
        scheme = "mssql";
        break;
    case DatabaseType::ORACLE:
        scheme = "oracle";
        break;
    default:
        return ""; // CASSANDRA and anything else: no URL form
    }

    std::string url = scheme;
    url += "://";

    if (!info.username.empty() || !info.password.empty()) {
        url += percentEncode(info.username);
        if (!info.password.empty()) {
            url.push_back(':');
            url += percentEncode(info.password);
        }
        url.push_back('@');
    }

    // IPv6 literals need [brackets]; assume a string containing ':' that is
    // not already bracketed is IPv6.
    if (!info.host.empty() && info.host.front() != '[' &&
        info.host.find(':') != std::string::npos) {
        url.push_back('[');
        url += info.host;
        url.push_back(']');
    } else {
        url += info.host;
    }

    if (info.port > 0) {
        url.push_back(':');
        url += std::to_string(info.port);
    }

    if (!info.database.empty()) {
        url.push_back('/');
        url += percentEncode(info.database);
    }

    // Query params. For redis/rediss, TLS is encoded in the scheme — skip
    // sslmode there.
    std::string query;
    auto addParam = [&](const std::string& k, const std::string& v) {
        if (!query.empty())
            query.push_back('&');
        query += k;
        query.push_back('=');
        query += v;
    };

    if (info.type != DatabaseType::REDIS) {
        if (info.sslmode != SslMode::Prefer) { // Prefer is the implicit default
            addParam("sslmode", sslModeToString(info.sslmode));
        }
    }
    if (!info.sslCACertPath.empty()) {
        addParam(info.type == DatabaseType::MONGODB ? "tlsCAFile" : "sslrootcert",
                 percentEncode(info.sslCACertPath));
    }

    if (!query.empty()) {
        url.push_back('?');
        url += query;
    }
    return url;
}

bool looksLikeConnectionUrl(const std::string& s) {
    auto pos = s.find("://");
    if (pos == std::string::npos || pos == 0)
        return false;
    return resolveScheme(std::string_view(s).substr(0, pos)).has_value();
}

ConnectionUrlParseResult parseConnectionUrl(const std::string& url) {
    ConnectionUrlParseResult r;

    // trim leading/trailing whitespace
    size_t b = 0, e = url.size();
    while (b < e && std::isspace(static_cast<unsigned char>(url[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(url[e - 1])))
        --e;
    std::string_view sv(url.data() + b, e - b);

    auto schemeEnd = sv.find("://");
    if (schemeEnd == std::string_view::npos) {
        r.error = "missing scheme (expected e.g. postgresql://)";
        return r;
    }

    auto schemeInfo = resolveScheme(sv.substr(0, schemeEnd));
    if (!schemeInfo) {
        r.error = "unsupported scheme '" + std::string(sv.substr(0, schemeEnd)) + "'";
        return r;
    }

    r.info.type = schemeInfo->type;
    r.info.port = schemeInfo->defaultPort;
    if (schemeInfo->tls) {
        r.info.sslmode = SslMode::Require;
    } else if (schemeInfo->type == DatabaseType::MONGODB ||
               schemeInfo->type == DatabaseType::REDIS) {
        // simple-ssl backends default to off; server backends keep their type default.
        r.info.sslmode = SslMode::Disable;
    }

    std::string_view rest = sv.substr(schemeEnd + 3);

    // file backends: everything after :// is the path (allow optional leading /).
    if (isFileDatabase(schemeInfo->type)) {
        std::string_view path = rest;
        if (!path.empty() && path.front() == '/' && path.size() > 1 && path[1] == '/') {
            // sqlite:////absolute → strip one slash
            path.remove_prefix(1);
        }
        if (!percentDecode(path, r.info.path)) {
            r.error = "malformed percent-escape in path";
            return r;
        }
        r.ok = true;
        return r;
    }

    // Split off query string.
    std::string_view query;
    if (auto q = rest.find('?'); q != std::string_view::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }

    // Split authority and path.
    std::string_view authority = rest;
    std::string_view path;
    if (auto slash = rest.find('/'); slash != std::string_view::npos) {
        authority = rest.substr(0, slash);
        path = rest.substr(slash + 1);
    }

    // Userinfo (split on rightmost '@' so '@' in passwords works when encoded).
    std::string_view userinfo;
    std::string_view hostport = authority;
    if (auto at = authority.rfind('@'); at != std::string_view::npos) {
        userinfo = authority.substr(0, at);
        hostport = authority.substr(at + 1);
    }

    if (!userinfo.empty()) {
        auto colon = userinfo.find(':');
        std::string_view u = colon == std::string_view::npos ? userinfo : userinfo.substr(0, colon);
        std::string_view p =
            colon == std::string_view::npos ? std::string_view{} : userinfo.substr(colon + 1);
        if (!percentDecode(u, r.info.username) || !percentDecode(p, r.info.password)) {
            r.error = "malformed percent-escape in userinfo";
            return r;
        }
    }

    // Host (allow [ipv6]) and port.
    if (!hostport.empty() && hostport.front() == '[') {
        auto rb = hostport.find(']');
        if (rb == std::string_view::npos) {
            r.error = "unterminated IPv6 host";
            return r;
        }
        r.info.host = std::string(hostport.substr(1, rb - 1));
        std::string_view tail = hostport.substr(rb + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') {
                r.error = "expected ':' after IPv6 host";
                return r;
            }
            tail.remove_prefix(1);
            if (!parsePort(tail, r.info.port)) {
                r.error = "invalid port";
                return r;
            }
        }
    } else {
        auto colon = hostport.find(':');
        if (colon == std::string_view::npos) {
            r.info.host = std::string(hostport);
        } else {
            r.info.host = std::string(hostport.substr(0, colon));
            std::string_view portStr = hostport.substr(colon + 1);
            if (!portStr.empty()) {
                if (!parsePort(portStr, r.info.port)) {
                    r.error = "invalid port";
                    return r;
                }
            }
        }
    }

    if (r.info.host.empty()) {
        r.error = "missing host";
        return r;
    }
    if (r.info.port <= 0 || r.info.port > 65535) {
        r.error = "port out of range";
        return r;
    }

    // Database name = path segment up to next '/'. Mongo replica-set paths and
    // Oracle service names sit in the same slot.
    if (!path.empty()) {
        auto nextSlash = path.find('/');
        std::string_view dbSlot =
            nextSlash == std::string_view::npos ? path : path.substr(0, nextSlash);
        if (!percentDecode(dbSlot, r.info.database)) {
            r.error = "malformed percent-escape in database";
            return r;
        }
    }

    // Query string: key=value pairs separated by '&'. Keys are case-insensitive.
    while (!query.empty()) {
        auto amp = query.find('&');
        std::string_view pair = amp == std::string_view::npos ? query : query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        if (pair.empty())
            continue;
        auto eq = pair.find('=');
        std::string keyRaw = std::string(eq == std::string_view::npos ? pair : pair.substr(0, eq));
        std::string valRaw =
            std::string(eq == std::string_view::npos ? std::string_view{} : pair.substr(eq + 1));
        std::string keyDec, valDec;
        if (!percentDecode(keyRaw, keyDec) || !percentDecode(valRaw, valDec)) {
            r.error = "malformed percent-escape in query string";
            return r;
        }
        applySslmodeFromQuery(r.info, toLower(keyDec), valDec);
    }

    r.ok = true;
    return r;
}
