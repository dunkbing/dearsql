#include "utils/table_importer.hpp"
#include "utils/logger.hpp"
#include <format>
#include <fstream>
#include <nfd.h>
#include <sstream>

namespace {

    std::string escapeSQL(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 2);
        out += '\'';
        for (const char c : value) {
            if (c == '\'')
                out += '\'';
            out += c;
        }
        out += '\'';
        return out;
    }

    // parse a single CSV field starting at pos, advance pos past the field and trailing comma
    std::string parseField(const std::string& line, size_t& pos) {
        if (pos >= line.size())
            return "";

        if (line[pos] == '"') {
            // quoted field
            ++pos;
            std::string field;
            while (pos < line.size()) {
                if (line[pos] == '"') {
                    ++pos;
                    if (pos < line.size() && line[pos] == '"') {
                        // escaped quote
                        field += '"';
                        ++pos;
                    } else {
                        break;
                    }
                } else {
                    field += line[pos++];
                }
            }
            if (pos < line.size() && line[pos] == ',')
                ++pos;
            return field;
        }

        // unquoted field
        size_t start = pos;
        while (pos < line.size() && line[pos] != ',')
            ++pos;
        std::string field = line.substr(start, pos - start);
        if (pos < line.size())
            ++pos;
        return field;
    }

    std::vector<std::string> parseLine(const std::string& line) {
        std::vector<std::string> fields;
        size_t pos = 0;
        while (pos <= line.size()) {
            fields.push_back(parseField(line, pos));
            if (pos > line.size())
                break;
        }
        // trim trailing empty field from a line that ends without comma
        if (!fields.empty() && fields.back().empty() && !line.empty() && line.back() != ',')
            fields.pop_back();
        return fields;
    }

} // namespace

namespace TableImporter {

    bool importFromCSV(IQueryExecutor* executor, const std::string& tableName) {
        if (!executor)
            return false;

        nfdfilteritem_t filter = {"CSV Files", "csv"};
        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_OpenDialog(&outPath, &filter, 1, nullptr);
        if (result != NFD_OKAY) {
            if (result == NFD_ERROR)
                Logger::error(std::format("File dialog error: {}", NFD_GetError()));
            return false;
        }

        std::string path(outPath);
        NFD_FreePath(outPath);

        std::ifstream file(path);
        if (!file.is_open()) {
            Logger::error(std::format("Failed to open file: {}", path));
            return false;
        }

        std::string headerLine;
        if (!std::getline(file, headerLine)) {
            Logger::error("CSV file is empty");
            return false;
        }
        // strip \r if present
        if (!headerLine.empty() && headerLine.back() == '\r')
            headerLine.pop_back();

        const auto columns = parseLine(headerLine);
        if (columns.empty()) {
            Logger::error("CSV header row has no columns");
            return false;
        }

        std::string colList;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0)
                colList += ", ";
            colList += columns[i];
        }

        int inserted = 0;
        int failed = 0;
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;

            const auto values = parseLine(line);

            std::string valueList;
            for (size_t i = 0; i < columns.size(); ++i) {
                if (i > 0)
                    valueList += ", ";
                const std::string& val = i < values.size() ? values[i] : "";
                valueList += val.empty() ? "NULL" : escapeSQL(val);
            }

            const std::string sql =
                std::format("INSERT INTO {} ({}) VALUES ({})", tableName, colList, valueList);

            auto queryResult = executor->executeQuery(sql);
            if (queryResult.success()) {
                ++inserted;
            } else {
                ++failed;
                Logger::error(std::format("Row {} insert failed: {}", inserted + failed,
                                          queryResult.errorMessage()));
            }
        }

        Logger::info(std::format("CSV import complete: {} inserted, {} failed — {}", inserted,
                                 failed, path));
        return failed == 0;
    }

} // namespace TableImporter
