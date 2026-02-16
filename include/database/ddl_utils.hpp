#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ddl_utils {

    struct ColumnType {
        std::string type = "TEXT";
        int precision = 0;
        int scale = 0;
    };

    inline std::string trim(std::string value) {
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                        return std::isspace(ch) == 0;
                    }));
        value.erase(std::find_if(value.rbegin(), value.rend(),
                                 [](unsigned char ch) { return std::isspace(ch) == 0; })
                        .base(),
                    value.end());
        return value;
    }

    inline std::string toUpper(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        return value;
    }

    inline std::pair<int, int> parsePrecisionScale(const std::string& type) {
        const auto open = type.find('(');
        if (open == std::string::npos) {
            return {0, 0};
        }
        const auto close = type.find(')', open + 1);
        if (close == std::string::npos || close <= open + 1) {
            return {0, 0};
        }
        std::string inside = type.substr(open + 1, close - open - 1);
        inside.erase(std::remove_if(inside.begin(), inside.end(),
                                    [](unsigned char ch) { return std::isspace(ch) != 0; }),
                     inside.end());

        const auto comma = inside.find(',');
        try {
            if (comma == std::string::npos) {
                return {std::stoi(inside), 0};
            }
            return {std::stoi(inside.substr(0, comma)), std::stoi(inside.substr(comma + 1))};
        } catch (const std::exception&) {
            return {0, 0};
        }
    }

    inline ColumnType inferColumnType(const std::string& rawType) {
        ColumnType result;
        std::string normalized = toUpper(trim(rawType));
        const auto [precision, scale] = parsePrecisionScale(normalized);

        auto has = [&normalized](std::string_view token) {
            return normalized.find(token) != std::string::npos;
        };

        if (has("TINYINT") || has("SMALLINT")) {
            result.type = "INTEGER";
        } else if (has("BIGINT")) {
            result.type = "BIGINT";
        } else if (has("INT") || has("INTEGER")) {
            result.type = "INTEGER";
        } else if (has("BOOL") || has("BOOLEAN")) {
            result.type = "INTEGER";
        } else if (has("DOUBLE") || has("FLOAT") || has("REAL")) {
            result.type = "REAL";
        } else if (has("DECIMAL") || has("NUMERIC")) {
            result.type = "REAL";
        } else if (has("DATE") || has("TIME")) {
            result.type = "TEXT";
        } else if (has("BLOB") || has("BYTEA") || has("BINARY")) {
            result.type = "BLOB";
        } else if (has("CHAR") || has("TEXT") || has("CLOB") || has("UUID") || has("JSON")) {
            result.type = "TEXT";
        } else {
            result.type = "TEXT";
        }

        result.precision = precision;
        result.scale = scale;
        return result;
    }

    inline std::string joinColumnNames(const std::vector<std::string>& names) {
        std::string joined;
        for (size_t i = 0; i < names.size(); ++i) {
            joined += names[i];
            if (i + 1 < names.size()) {
                joined += ", ";
            }
        }
        return joined;
    }

    inline std::string escapeSingleQuotes(const std::string& value) {
        std::string escaped;
        escaped.reserve(value.size());
        for (char ch : value) {
            if (ch == '\'') {
                escaped += "''";
            } else {
                escaped += ch;
            }
        }
        return escaped;
    }

    inline std::string makeConstraintName(std::string_view prefix, const std::string& tableName) {
        std::string name(prefix);
        name += tableName;
        std::replace(name.begin(), name.end(), '.', '_');
        return name;
    }

} // namespace ddl_utils
