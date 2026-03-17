#pragma once

#include <csv2/reader.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace CsvParser {

    using CsvReader =
        csv2::Reader<csv2::delimiter<','>, csv2::quote_character<'"'>,
                     csv2::first_row_is_header<true>, csv2::trim_policy::trim_whitespace>;

    // quote a field for csv output (csv2::Writer doesn't do quoting itself)
    inline std::string quoteField(const std::string& value) {
        const bool needsQuoting = value.find_first_of(",\"\n\r") != std::string::npos;
        if (!needsQuoting)
            return value;

        std::string result = "\"";
        for (char c : value) {
            if (c == '"')
                result += "\"\"";
            else
                result += c;
        }
        result += '"';
        return result;
    }

    template <typename Reader>
    inline bool readParsed(Reader& reader, std::vector<std::string>& headers,
                           std::vector<std::vector<std::string>>& rows) {
        headers.clear();
        rows.clear();

        // read header
        const auto header = reader.header();
        for (const auto& cell : header) {
            std::string value;
            cell.read_value(value);
            headers.push_back(value);
        }

        if (headers.empty())
            return true;

        // read data rows
        for (const auto& row : reader) {
            std::vector<std::string> rowValues;
            rowValues.reserve(headers.size());
            for (const auto& cell : row) {
                std::string value;
                cell.read_value(value);
                rowValues.push_back(value);
            }
            // pad or trim to match header count
            rowValues.resize(headers.size());
            rows.push_back(std::move(rowValues));
        }

        return true;
    }

    inline bool parseText(const std::string& text, std::vector<std::string>& headers,
                          std::vector<std::vector<std::string>>& rows) {
        headers.clear();
        rows.clear();

        if (text.empty())
            return true;

        CsvReader reader;
        if (!reader.parse(text))
            return false;

        return readParsed(reader, headers, rows);
    }

    // parse a csv file into headers + rows using csv2::Reader (mmap-based)
    inline bool parseFile(const std::string& path, std::vector<std::string>& headers,
                          std::vector<std::vector<std::string>>& rows) {
        std::error_code ec;
        if (std::filesystem::file_size(path, ec) == 0 && !ec) {
            headers.clear();
            rows.clear();
            return true;
        }

        CsvReader reader;
        if (reader.mmap(path))
            return readParsed(reader, headers, rows);

        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
            return false;

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return parseText(buffer.str(), headers, rows);
    }

    inline void writeRow(std::ostream& stream, const std::vector<std::string>& row) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0)
                stream << ',';
            stream << quoteField(row[i]);
        }
        stream << '\n';
    }

    // write headers + rows to a csv file
    inline bool writeFile(const std::string& path, const std::vector<std::string>& headers,
                          const std::vector<std::vector<std::string>>& rows) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open())
            return false;

        if (!headers.empty())
            writeRow(stream, headers);

        for (const auto& row : rows)
            writeRow(stream, row);

        return stream.good();
    }

    // serialize headers + rows to a raw string (for the raw view)
    inline std::string serialize(const std::vector<std::string>& headers,
                                 const std::vector<std::vector<std::string>>& rows) {
        std::ostringstream out;
        if (!headers.empty()) {
            for (size_t i = 0; i < headers.size(); ++i) {
                if (i > 0)
                    out << ',';
                out << quoteField(headers[i]);
            }
            out << '\n';
        }

        for (size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
            const auto& row = rows[rowIndex];
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0)
                    out << ',';
                out << quoteField(row[i]);
            }
            if (rowIndex + 1 < rows.size())
                out << '\n';
        }
        return out.str();
    }

} // namespace CsvParser
