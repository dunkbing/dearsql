#pragma once

#include <cctype>
#include <string>
#include <string_view>

// ponytail: first-keyword allowlist + write-keyword denylist per statement; no parser.
// blocks the obvious write paths, not a security boundary against a hostile agent
namespace SqlGuard {

    // blank out string literals and comments so they cannot hide a ';' or keyword
    inline std::string stripLiterals(std::string_view sql) {
        std::string out(sql.size(), ' ');
        for (size_t i = 0; i < sql.size();) {
            const char c = sql[i];
            if (c == '\'' || c == '"' || c == '`') {
                size_t j = sql.find(c, i + 1);
                i = j == std::string_view::npos ? sql.size() : j + 1;
            } else if (sql.substr(i, 2) == "--") {
                size_t j = sql.find('\n', i);
                i = j == std::string_view::npos ? sql.size() : j; // keep the newline
            } else if (sql.substr(i, 2) == "/*") {
                size_t j = sql.find("*/", i + 2);
                i = j == std::string_view::npos ? sql.size() : j + 2;
            } else {
                out[i] = c;
                ++i;
            }
        }
        return out;
    }

    inline bool isWordChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    inline bool isReadOnly(std::string_view sql) {
        auto allowedFirst = [](const std::string& kw) {
            return kw == "select" || kw == "show" || kw == "explain" || kw == "describe" ||
                   kw == "desc" || kw == "with" || kw == "pragma" || kw == "values" ||
                   kw == "table";
        };
        auto denied = [](const std::string& kw) {
            return kw == "insert" || kw == "update" || kw == "delete" || kw == "truncate" ||
                   kw == "drop" || kw == "alter" || kw == "grant" || kw == "revoke" ||
                   kw == "merge" || kw == "into" || kw == "analyze" || kw == "analyse" ||
                   kw == "vacuum";
        };

        const std::string clean = stripLiterals(sql);
        bool sawStatement = false;
        auto checkStatement = [&](std::string_view stmt) {
            bool first = true;
            for (size_t i = 0; i < stmt.size();) {
                if (!isWordChar(stmt[i])) {
                    ++i;
                    continue;
                }
                std::string word;
                while (i < stmt.size() && isWordChar(stmt[i])) {
                    word += static_cast<char>(std::tolower(static_cast<unsigned char>(stmt[i])));
                    ++i;
                }
                if (first) {
                    sawStatement = true;
                    if (!allowedFirst(word)) {
                        return false;
                    }
                    first = false;
                } else if (denied(word)) {
                    return false;
                }
            }
            return true;
        };

        size_t start = 0;
        for (size_t i = 0; i <= clean.size(); ++i) {
            if (i == clean.size() || clean[i] == ';') {
                if (!checkStatement(std::string_view(clean).substr(start, i - start))) {
                    return false;
                }
                start = i + 1;
            }
        }
        return sawStatement;
    }

} // namespace SqlGuard
