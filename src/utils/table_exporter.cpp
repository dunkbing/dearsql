#include "utils/table_exporter.hpp"
#include "database/ddl_utils.hpp"
#include <filesystem>
#include <fstream>
#include <nfd.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

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
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto columns = provider->getColumnNames(tableName);
        if (columns.empty()) {
            spdlog::error("Cannot export: table has no columns");
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

        spdlog::info("Exported {} rows to CSV: {}", totalRows, path);
        return true;
    }

    bool exportJson(ITableDataProvider* provider, const std::string& tableName,
                    const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto columns = provider->getColumnNames(tableName);
        if (columns.empty()) {
            spdlog::error("Cannot export: table has no columns");
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

        spdlog::info("Exported {} rows to JSON: {}", totalRows, path);
        return true;
    }

    std::string quoteSqlValue(const std::string& value) {
        if (value == "NULL")
            return "NULL";
        return "'" + ddl_utils::escapeSingleQuotes(value) + "'";
    }

    void writeCreateTable(std::ofstream& file, const Table& table) {
        file << "CREATE TABLE " << table.name << " (\n";

        std::vector<std::string> pkColumns;
        for (const auto& col : table.columns) {
            if (col.isPrimaryKey)
                pkColumns.push_back(col.name);
        }

        for (size_t i = 0; i < table.columns.size(); ++i) {
            const auto& col = table.columns[i];
            file << "    " << col.name << " " << col.type;
            if (col.isNotNull)
                file << " NOT NULL";
            if (col.isUnique && !col.isPrimaryKey)
                file << " UNIQUE";
            if (!col.defaultValue.empty())
                file << " DEFAULT " << col.defaultValue;
            // inline PRIMARY KEY only for single-column PKs
            if (col.isPrimaryKey && pkColumns.size() == 1)
                file << " PRIMARY KEY";
            if (i + 1 < table.columns.size() || pkColumns.size() > 1)
                file << ',';
            file << '\n';
        }

        if (pkColumns.size() > 1) {
            file << "    PRIMARY KEY (";
            for (size_t i = 0; i < pkColumns.size(); ++i) {
                if (i > 0)
                    file << ", ";
                file << pkColumns[i];
            }
            file << ")\n";
        }

        file << ");\n\n";
    }

    bool exportSql(ITableDataProvider* provider, const Table& table, const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto columns = provider->getColumnNames(table.name);
        if (columns.empty()) {
            spdlog::error("Cannot export: table has no columns");
            return false;
        }

        if (!table.columns.empty())
            writeCreateTable(file, table);

        // build column list for INSERTs
        std::string columnList;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0)
                columnList += ", ";
            columnList += columns[i];
        }

        int totalRows = provider->getRowCount(table.name);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table.name, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                file << "INSERT INTO " << table.name << " (" << columnList << ") VALUES (";
                for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                    if (i > 0)
                        file << ", ";
                    file << quoteSqlValue(row[i]);
                }
                file << ");\n";
            }
        }

        spdlog::info("Exported {} rows to SQL: {}", totalRows, path);
        return true;
    }

    constexpr std::string_view PORTAL_CANCEL_MSG = "response code 2";

    bool isPortalCancel() {
        const char* err = NFD_GetError();
        return err && std::string_view(err).find(PORTAL_CANCEL_MSG) != std::string_view::npos;
    }

    std::string showFolderDialog() {
        nfdchar_t* outPath = nullptr;
        // use save dialog so the user can type a folder name
        const nfdresult_t result = NFD_SaveDialog(&outPath, nullptr, 0, nullptr, "data");
        if (result == NFD_OKAY) {
            std::string path(outPath);
            NFD_FreePath(outPath);
            return path;
        }
        if (result == NFD_ERROR && !isPortalCancel()) {
            spdlog::error("Folder dialog error: {}", NFD_GetError());
        }
        return "";
    }

    std::string showSaveDialog(ExportFormat format, const std::string& tableName) {
        const char* ext = nullptr;
        const char* desc = nullptr;
        switch (format) {
        case ExportFormat::CSV:
            ext = "csv";
            desc = "CSV Files";
            break;
        case ExportFormat::JSON:
            ext = "json";
            desc = "JSON Files";
            break;
        case ExportFormat::SQL:
            ext = "sql";
            desc = "SQL Files";
            break;
        }
        nfdfilteritem_t filter = {desc, ext};

        std::string defaultName = tableName + "." + ext;

        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_SaveDialog(&outPath, &filter, 1, nullptr, defaultName.c_str());
        if (result == NFD_OKAY) {
            std::string path(outPath);
            NFD_FreePath(outPath);
            return path;
        }
        if (result == NFD_ERROR && !isPortalCancel()) {
            spdlog::error("File dialog error: {}", NFD_GetError());
        }
        return "";
    }

} // namespace

namespace TableExporter {

    bool exportTables(ITableDataProvider* provider, const std::vector<const Table*>& tables,
                      ExportFormat format) {
        if (!provider || tables.empty()) {
            return false;
        }

        const char* ext = nullptr;
        switch (format) {
        case ExportFormat::CSV:
            ext = "csv";
            break;
        case ExportFormat::JSON:
            ext = "json";
            break;
        case ExportFormat::SQL:
            ext = "sql";
            break;
        }

        if (tables.size() == 1) {
            const std::string path = showSaveDialog(format, tables[0]->name);
            if (path.empty()) {
                return false;
            }
            switch (format) {
            case ExportFormat::CSV:
                return exportCsv(provider, tables[0]->name, path);
            case ExportFormat::JSON:
                return exportJson(provider, tables[0]->name, path);
            case ExportFormat::SQL:
                return exportSql(provider, *tables[0], path);
            }
            return false;
        }

        const std::string folder = showFolderDialog();
        if (folder.empty()) {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        if (ec) {
            spdlog::error("Failed to create export folder '{}': {}", folder, ec.message());
            return false;
        }

        bool allOk = true;
        for (const Table* table : tables) {
            const std::string path =
                (std::filesystem::path(folder) / (table->name + "." + ext)).string();
            bool ok = false;
            switch (format) {
            case ExportFormat::CSV:
                ok = exportCsv(provider, table->name, path);
                break;
            case ExportFormat::JSON:
                ok = exportJson(provider, table->name, path);
                break;
            case ExportFormat::SQL:
                ok = exportSql(provider, *table, path);
                break;
            }
            if (!ok) {
                allOk = false;
            }
        }
        return allOk;
    }

} // namespace TableExporter
