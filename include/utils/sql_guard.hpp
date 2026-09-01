#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

// ponytail: keyword allowlist on the first token of each statement; no parser.
// blocks the obvious write paths, not a security boundary against a hostile agent
namespace SqlGuard {

    inline std::string firstKeyword(std::string_view sql) {
        size_t i = 0;
        // skip whitespace and leading comments
        while (i < sql.size()) {
            if (std::isspace(static_cast<unsigned char>(sql[i]))) {
                ++i;
            } else if (sql.substr(i, 2) == "--") {
                const auto nl = sql.find('\n', i);
                if (nl == std::string_view::npos) {
                    return "";
                }
                i = nl + 1;
            } else if (sql.substr(i, 2) == "/*") {
                const auto end = sql.find("*/", i + 2);
                if (end == std::string_view::npos) {
                    return "";
                }
                i = end + 2;
            } else {
                break;
            }
        }
        std::string word;
        while (i < sql.size() &&
               (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
            word += static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i])));
            ++i;
        }
        return word;
    }

    // true when every statement in `sql` starts with a read-only keyword
    inline bool isReadOnly(std::string_view sql) {
        auto allowed = [](const std::string& kw) {
            return kw == "select" || kw == "show" || kw == "explain" || kw == "describe" ||
                   kw == "desc" || kw == "with" || kw == "pragma" || kw == "values" ||
                   kw == "table";
        };

        // split on top-level semicolons (quote-aware, good enough)
        size_t start = 0;
        bool inSingle = false, inDouble = false;
        bool sawStatement = false;
        auto checkRange = [&](size_t from, size_t to) {
            const auto stmt = sql.substr(from, to - from);
            const std::string kw = firstKeyword(stmt);
            if (kw.empty()) {
                return true; // blank tail
            }
            sawStatement = true;
            return allowed(kw);
        };
        for (size_t i = 0; i < sql.size(); ++i) {
            const char c = sql[i];
            if (c == '\'' && !inDouble) {
                inSingle = !inSingle;
            } else if (c == '"' && !inSingle) {
                inDouble = !inDouble;
            } else if (c == ';' && !inSingle && !inDouble) {
                if (!checkRange(start, i)) {
                    return false;
                }
                start = i + 1;
            }
        }
        if (!checkRange(start, sql.size())) {
            return false;
        }
        return sawStatement;
    }

} // namespace SqlGuard
