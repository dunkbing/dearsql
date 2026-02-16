#include "database/db.hpp"
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

void buildForeignKeyLookup(Table& table) {
    table.foreignKeysByColumn.clear();
    for (const auto& fk : table.foreignKeys) {
        table.foreignKeysByColumn[fk.sourceColumn] = fk;
    }
}

void populateIncomingForeignKeys(std::vector<Table>& tables) {
    std::unordered_map<std::string, Table*> tableLookup;
    tableLookup.reserve(tables.size());

    for (auto& table : tables) {
        table.incomingForeignKeys.clear();
        tableLookup[table.name] = &table;
    }

    for (const auto& sourceTable : tables) {
        for (const auto& fk : sourceTable.foreignKeys) {
            if (const auto targetIt = tableLookup.find(fk.targetTable);
                targetIt != tableLookup.end()) {
                ForeignKey incoming = fk;
                incoming.targetTable = sourceTable.name; // Table referencing the target
                incoming.sourceColumn = fk.sourceColumn;
                incoming.targetColumn = fk.targetColumn;
                incoming.onDelete = fk.onDelete;
                incoming.onUpdate = fk.onUpdate;
                targetIt->second->incomingForeignKeys.push_back(std::move(incoming));
            }
        }
    }
}

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
