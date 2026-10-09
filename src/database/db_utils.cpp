#include "database/db.hpp"
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sql {
    std::string and_(const std::vector<std::string>& conditions) {
        if (conditions.empty()) {
            return "";
        }
        if (conditions.size() == 1) {
            return conditions[0];
        }

        std::ostringstream oss;
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) {
                oss << " AND ";
            }
            oss << "(" << conditions[i] << ")";
        }
        return oss.str();
    }

    std::string or_(const std::vector<std::string>& conditions) {
        if (conditions.empty()) {
            return "";
        }
        if (conditions.size() == 1) {
            return conditions[0];
        }

        std::ostringstream oss;
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) {
                oss << " OR ";
            }
            oss << "(" << conditions[i] << ")";
        }
        return oss.str();
    }

    std::string eq(const std::string& column, const std::string& value) {
        return column + " = " + value;
    }

    std::string like(const std::string& column, const std::string& pattern) {
        return column + " LIKE " + pattern;
    }

    std::string ilike(const std::string& column, const std::string& pattern) {
        return column + " ILIKE " + pattern;
    }
} // namespace sql
