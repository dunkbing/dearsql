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

    // a pipe would end the cell and a newline the row, so both are escaped
    std::string escapeMarkdownCell(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (const char c : value) {
            if (c == '|')
                out += "\\|";
            else if (c == '\n')
                out += "<br>";
            else if (c != '\r')
                out += c;
        }
        return out;
    }

    std::string escapeHtml(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        for (const char c : value) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
            }
        }
        return out;
    }

    // cell text shared by the markdown and html writers; null reads as an empty
    // cell in both, the way the grid shows it
    std::string displayValue(const std::string& raw) {
        if (isNullSentinel(raw))
            return "";
        if (isBoolSentinel(raw))
            return boolSentinelValue(raw) ? "true" : "false";
        return raw;
    }

    bool exportMarkdown(ITableDataProvider* provider, const Table& table, const std::string& path) {
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

        for (const auto& col : columns)
            file << "| " << escapeMarkdownCell(col) << ' ';
        file << "|\n";
        for (size_t i = 0; i < columns.size(); ++i)
            file << "| --- ";
        file << "|\n";

        const int totalRows = provider->getRowCount(table);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                for (size_t i = 0; i < columns.size(); ++i) {
                    const std::string cell = i < row.size() ? displayValue(row[i]) : "";
                    file << "| " << escapeMarkdownCell(cell) << ' ';
                }
                file << "|\n";
            }
        }

        spdlog::info("Exported {} rows to Markdown: {}", totalRows, path);
        return true;
    }

    bool exportHtml(ITableDataProvider* provider, const Table& table, const std::string& path) {
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

        // a standalone document rather than a bare fragment, so it opens in a
        // browser and still pastes into a document as a table
        file << "<!doctype html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>"
             << escapeHtml(table.name) << "</title>\n<style>\n"
             << "body{font-family:system-ui,sans-serif;font-size:14px;margin:2rem}\n"
             << "table{border-collapse:collapse}\n"
             << "th,td{border:1px solid #ccc;padding:.35rem .6rem;text-align:left}\n"
             << "th{background:#f4f4f4}\n</style>\n</head>\n<body>\n<table>\n<thead>\n<tr>";
        for (const auto& col : columns)
            file << "<th>" << escapeHtml(col) << "</th>";
        file << "</tr>\n</thead>\n<tbody>\n";

        const int totalRows = provider->getRowCount(table);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                file << "<tr>";
                for (size_t i = 0; i < columns.size(); ++i) {
                    const std::string cell = i < row.size() ? displayValue(row[i]) : "";
                    file << "<td>" << escapeHtml(cell) << "</td>";
                }
                file << "</tr>\n";
            }
        }
        file << "</tbody>\n</table>\n</body>\n</html>\n";

        spdlog::info("Exported {} rows to HTML: {}", totalRows, path);
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

        int totalRows = provider->getRowCount(table);
        for (int offset = 0; offset < totalRows; offset += BATCH_SIZE) {
            auto rows = provider->getTableData(table, BATCH_SIZE, offset);
            for (const auto& row : rows) {
                std::vector<std::string> valueLiterals;
                valueLiterals.reserve(columns.size());
                for (size_t i = 0; i < columns.size(); ++i) {
                    valueLiterals.push_back(i < row.size() ? quoteSqlValue(row[i]) : "NULL");
                }
                file << builder.insertRow(quotedName, columns, valueLiterals) << ";\n";
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
        case ExportFormat::MARKDOWN:
            ext = "md";
            desc = "Markdown Files";
            break;
        case ExportFormat::HTML:
            ext = "html";
            desc = "HTML Files";
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
        case ExportFormat::MARKDOWN:
            ext = "md";
            break;
        case ExportFormat::HTML:
            ext = "html";
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
            case ExportFormat::MARKDOWN:
                return exportMarkdown(provider, *tables[0], path);
            case ExportFormat::HTML:
                return exportHtml(provider, *tables[0], path);
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
            case ExportFormat::MARKDOWN:
                ok = exportMarkdown(provider, *table, path);
                break;
            case ExportFormat::HTML:
                ok = exportHtml(provider, *table, path);
                break;
            default:
                break;
            }
            if (!ok) {
                allOk = false;
            }
        }
        return allOk;
    }

} // namespace TableExporter
