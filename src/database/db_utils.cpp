#include "database/db.hpp"
#include <soci/soci.h>
#include <string>

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
