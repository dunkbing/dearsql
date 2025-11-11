#include "database/db.hpp"
#include <soci/soci.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

std::string convertRowValue(const soci::row& row, const std::size_t columnIndex) {
    if (row.get_indicator(columnIndex) == soci::i_null) {
        return "NULL";
    }

    switch (const soci::column_properties& cp = row.get_properties(columnIndex); cp.get_db_type()) {
    case soci::db_string:
        return row.get<std::string>(columnIndex);
    case soci::db_wstring: {
        auto ws = row.get<std::wstring>(columnIndex);
        return {ws.begin(), ws.end()};
    }
    case soci::db_int8:
        return std::to_string(row.get<int8_t>(columnIndex));
    case soci::db_uint8:
        return std::to_string(row.get<uint8_t>(columnIndex));
    case soci::db_int16:
        return std::to_string(row.get<int16_t>(columnIndex));
    case soci::db_uint16:
        return std::to_string(row.get<uint16_t>(columnIndex));
    case soci::db_int32:
        return std::to_string(row.get<int32_t>(columnIndex));
    case soci::db_uint32:
        return std::to_string(row.get<uint32_t>(columnIndex));
    case soci::db_int64:
        return std::to_string(row.get<int64_t>(columnIndex));
    case soci::db_uint64:
        return std::to_string(row.get<uint64_t>(columnIndex));
    case soci::db_double:
        return std::to_string(row.get<double>(columnIndex));
    case soci::db_date: {
        const auto date = row.get<std::tm>(columnIndex);
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &date);
        return {buffer};
    }
    case soci::db_blob:
        return "[BINARY DATA]";
    case soci::db_xml:
        try {
            return row.get<std::string>(columnIndex);
        } catch (const std::bad_cast&) {
            return "[XML DATA]";
        }
    default:
        try {
            return row.get<std::string>(columnIndex);
        } catch (const std::bad_cast&) {
            return "[UNKNOWN DATA TYPE]";
        }
    }
}

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
