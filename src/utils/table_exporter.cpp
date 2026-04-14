#include "utils/table_exporter.hpp"
#include "database/db.hpp"
#include "database/ddl_utils.hpp"
#include "database/sql_builder.hpp"
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

    bool exportCsv(ITableDataProvider* provider, const Table& table, const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto columns = provider->getColumnNames(table);
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
        int totalRows = provider->getRowCount(table);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                    if (i > 0)
                        file << ',';
                    if (isNullSentinel(row[i]))
                        file << "";
                    else if (isBoolSentinel(row[i]))
                        file << (boolSentinelValue(row[i]) ? "true" : "false");
                    else
                        file << escapeCsvField(row[i]);
                }
                file << '\n';
            }
        }

        spdlog::info("Exported {} rows to CSV: {}", totalRows, path);
        return true;
    }

    bool exportJson(ITableDataProvider* provider, const Table& table, const std::string& path) {
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto columns = provider->getColumnNames(table);
        if (columns.empty()) {
            spdlog::error("Cannot export: table has no columns");
            return false;
        }
        int totalRows = provider->getRowCount(table);

        file << "[\n";
        bool firstRow = true;
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                if (!firstRow) {
                    file << ",\n";
                }
                firstRow = false;

                nlohmann::ordered_json obj;
                for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                    if (isNullSentinel(row[i])) {
                        obj[columns[i]] = nullptr;
                    } else if (isBoolSentinel(row[i])) {
                        obj[columns[i]] = boolSentinelValue(row[i]);
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
        if (isNullSentinel(value))
            return "NULL";
        if (isBoolSentinel(value))
            return boolSentinelValue(value) ? "TRUE" : "FALSE";
        return "'" + ddl_utils::escapeSingleQuotes(value) + "'";
    }

    void writeSqlTable(std::ofstream& file, ITableDataProvider* provider, const Table& table,
                       const ISQLBuilder& builder) {
        auto columns = provider->getColumnNames(table);
        if (columns.empty()) {
            spdlog::error("Cannot export: table '{}' has no columns", table.name);
            return;
        }

        if (!table.columns.empty())
            file << builder.createTable(table) << ";\n\n";

        auto quotedName = builder.quoteIdentifier(table.name);

        // build column list for INSERTs
        std::string columnList;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0)
                columnList += ", ";
            columnList += builder.quoteIdentifier(columns[i]);
        }

        int totalRows = provider->getRowCount(table);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                file << "INSERT INTO " << quotedName << " (" << columnList << ") VALUES (";
                for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                    if (i > 0)
                        file << ", ";
                    file << quoteSqlValue(row[i]);
                }
                file << ");\n";
            }
        }

        spdlog::info("Exported {} rows for table '{}'", totalRows, table.name);
    }

    bool exportSql(ITableDataProvider* provider, const Table& table, const std::string& path,
                   DatabaseType dbType) {
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto builder = createSQLBuilder(dbType);
        writeSqlTable(file, provider, table, *builder);
        return true;
    }

    bool exportSqlMulti(ITableDataProvider* provider, const std::vector<const Table*>& tables,
                        const std::string& path, DatabaseType dbType) {
        std::ofstream file(path);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", path);
            return false;
        }

        auto builder = createSQLBuilder(dbType);
        for (size_t i = 0; i < tables.size(); ++i) {
            if (i > 0)
                file << "\n";
            writeSqlTable(file, provider, *tables[i], *builder);
        }

        spdlog::info("Exported {} tables to SQL: {}", tables.size(), path);
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
                      ExportFormat format, DatabaseType dbType) {
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

        // SQL multi-table: single file
        if (format == ExportFormat::SQL && tables.size() > 1) {
            const std::string path = showSaveDialog(format, "export");
            if (path.empty())
                return false;
            return exportSqlMulti(provider, tables, path, dbType);
        }

        if (tables.size() == 1) {
            const std::string path = showSaveDialog(format, tables[0]->name);
            if (path.empty()) {
                return false;
            }
            switch (format) {
            case ExportFormat::CSV:
                return exportCsv(provider, *tables[0], path);
            case ExportFormat::JSON:
                return exportJson(provider, *tables[0], path);
            case ExportFormat::SQL:
                return exportSql(provider, *tables[0], path, dbType);
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
                ok = exportCsv(provider, *table, path);
                break;
            case ExportFormat::JSON:
                ok = exportJson(provider, *table, path);
                break;
            }
            if (!ok) {
                allOk = false;
            }
        }
        return allOk;
    }

} // namespace TableExporter
