#include "utils/table_exporter.hpp"
#include "utils/logger.hpp"
#include <format>
#include <fstream>
#include <nfd.h>
#include <nlohmann/json.hpp>

namespace {

    constexpr int BATCH_SIZE = 10000;

    std::string escapeCsvField(const std::string& field) {
        if (field.find_first_of(",\"\r\n") == std::string::npos) {
            return field;
        }
        std::string escaped = "\"";
        for (const char c : field) {
            if (c == '"') {
                escaped += "\"\"";
            } else {
                escaped += c;
            }
        }
        escaped += '"';
        return escaped;
    }

    bool exportCsv(ITableDataProvider* provider, const std::string& tableName,
                   const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) {
            Logger::error(std::format("Failed to open file for writing: {}", path));
            return false;
        }

        auto columns = provider->getColumnNames(tableName);
        if (columns.empty()) {
            Logger::error("Cannot export: table has no columns");
            return false;
        }

        // header
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0)
                file << ',';
            file << escapeCsvField(columns[i]);
        }
        file << '\n';

        // rows in batches
        int totalRows = provider->getRowCount(tableName);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(tableName, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                    if (i > 0)
                        file << ',';
                    file << escapeCsvField(row[i]);
                }
                file << '\n';
            }
        }

        Logger::info(std::format("Exported {} rows to CSV: {}", totalRows, path));
        return true;
    }

    bool exportJson(ITableDataProvider* provider, const std::string& tableName,
                    const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) {
            Logger::error(std::format("Failed to open file for writing: {}", path));
            return false;
        }

        auto columns = provider->getColumnNames(tableName);
        if (columns.empty()) {
            Logger::error("Cannot export: table has no columns");
            return false;
        }
        int totalRows = provider->getRowCount(tableName);

        file << "[\n";
        bool firstRow = true;
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(tableName, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                if (!firstRow) {
                    file << ",\n";
                }
                firstRow = false;

                nlohmann::ordered_json obj;
                for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                    if (row[i] == "NULL") {
                        obj[columns[i]] = nullptr;
                    } else {
                        obj[columns[i]] = row[i];
                    }
                }
                file << "  " << obj.dump();
            }
        }
        file << "\n]\n";

        Logger::info(std::format("Exported {} rows to JSON: {}", totalRows, path));
        return true;
    }

    std::string showSaveDialog(ExportFormat format, const std::string& tableName) {
        const char* ext = (format == ExportFormat::CSV) ? "csv" : "json";
        const char* desc = (format == ExportFormat::CSV) ? "CSV Files" : "JSON Files";
        nfdfilteritem_t filter = {desc, ext};

        std::string defaultName = tableName + "." + ext;

        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_SaveDialog(&outPath, &filter, 1, nullptr, defaultName.c_str());
        if (result == NFD_OKAY) {
            std::string path(outPath);
            NFD_FreePath(outPath);
            return path;
        }
        if (result == NFD_ERROR) {
            Logger::error(std::format("File dialog error: {}", NFD_GetError()));
        }
        return "";
    }

} // namespace

namespace TableExporter {

    bool exportTable(ITableDataProvider* provider, const std::string& tableName,
                     ExportFormat format) {
        if (!provider) {
            return false;
        }

        std::string path = showSaveDialog(format, tableName);
        if (path.empty()) {
            return false;
        }

        if (format == ExportFormat::CSV) {
            return exportCsv(provider, tableName, path);
        }
        return exportJson(provider, tableName, path);
    }

} // namespace TableExporter
