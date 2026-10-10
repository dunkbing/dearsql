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
    std::string quoteSqlValue(const std::string& value) {
        if (isNullSentinel(value))
            return "NULL";
        if (isBoolSentinel(value))
            return boolSentinelValue(value) ? "TRUE" : "FALSE";
        return "'" + ddl_utils::escapeSingleQuotes(value) + "'";
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

    const char* extensionOf(ExportFormat format) {
        switch (format) {
        case ExportFormat::CSV:
            return "csv";
        case ExportFormat::JSON:
            return "json";
        case ExportFormat::SQL:
            return "sql";
        case ExportFormat::MARKDOWN:
            return "md";
        case ExportFormat::HTML:
            return "html";
        }
        return "txt";
    }

    // one table into an open stream; the format decides the header, rows and footer
    class TableWriter {
    public:
        TableWriter(ITableDataProvider* provider, TableExporter::Progress& progress)
            : provider_(provider), progress_(progress) {}

        // false with error set when the table cannot be read; cancelled is separate
        bool write(std::ofstream& file, const Table& table, ExportFormat format,
                   const ISQLBuilder* builder, std::string& error) {
            const auto columns = provider_->getColumnNames(table);
            if (columns.empty()) {
                error = std::format("Table '{}' has no columns (or could not be read)", table.name);
                return false;
            }
            switch (format) {
            case ExportFormat::CSV:
                for (size_t i = 0; i < columns.size(); ++i)
                    file << (i > 0 ? "," : "") << escapeCsvField(columns[i]);
                file << '\n';
                forEachRow(table, [&](const std::vector<std::string>& row) {
                    for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                        if (i > 0)
                            file << ',';
                        if (isBoolSentinel(row[i]))
                            file << (boolSentinelValue(row[i]) ? "true" : "false");
                        else if (!isNullSentinel(row[i]))
                            file << escapeCsvField(row[i]);
                    }
                    file << '\n';
                });
                break;
            case ExportFormat::JSON: {
                file << "[\n";
                bool first = true;
                forEachRow(table, [&](const std::vector<std::string>& row) {
                    if (!first)
                        file << ",\n";
                    first = false;
                    nlohmann::ordered_json obj;
                    for (size_t i = 0; i < columns.size() && i < row.size(); ++i) {
                        if (isNullSentinel(row[i]))
                            obj[columns[i]] = nullptr;
                        else if (isBoolSentinel(row[i]))
                            obj[columns[i]] = boolSentinelValue(row[i]);
                        else
                            obj[columns[i]] = row[i];
                    }
                    file << "  " << obj.dump();
                });
                file << "\n]\n";
                break;
            }
            case ExportFormat::MARKDOWN:
                for (const auto& col : columns)
                    file << "| " << escapeMarkdownCell(col) << ' ';
                file << "|\n";
                for (size_t i = 0; i < columns.size(); ++i)
                    file << "| --- ";
                file << "|\n";
                forEachRow(table, [&](const std::vector<std::string>& row) {
                    for (size_t i = 0; i < columns.size(); ++i) {
                        const std::string cell = i < row.size() ? displayValue(row[i]) : "";
                        file << "| " << escapeMarkdownCell(cell) << ' ';
                    }
                    file << "|\n";
                });
                break;
            case ExportFormat::HTML:
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
                forEachRow(table, [&](const std::vector<std::string>& row) {
                    file << "<tr>";
                    for (size_t i = 0; i < columns.size(); ++i) {
                        const std::string cell = i < row.size() ? displayValue(row[i]) : "";
                        file << "<td>" << escapeHtml(cell) << "</td>";
                    }
                    file << "</tr>\n";
                });
                file << "</tbody>\n</table>\n</body>\n</html>\n";
                break;
            case ExportFormat::SQL: {
                if (!table.columns.empty())
                    file << builder->createTable(table) << ";\n\n";
                const auto quotedName = builder->quoteIdentifier(table.name);
                forEachRow(table, [&](const std::vector<std::string>& row) {
                    std::vector<std::string> literals;
                    literals.reserve(columns.size());
                    for (size_t i = 0; i < columns.size(); ++i)
                        literals.push_back(i < row.size() ? quoteSqlValue(row[i]) : "NULL");
                    file << builder->insertRow(quotedName, columns, literals) << ";\n";
                });
                break;
            }
            }
            return true;
        }

    private:
        // pages through the table; stops early on cancel or an empty page (the
        // provider reports read errors as no rows)
        template <typename F> void forEachRow(const Table& table, F&& onRow) {
            const int total = provider_->getRowCount(table);
            progress_.rowsTotal.store(std::max(total, 0), std::memory_order_relaxed);
            progress_.tableRowsWritten.store(0, std::memory_order_relaxed);
            for (int offset = 0; offset < total; offset += BATCH_SIZE) {
                if (progress_.cancelRequested.load(std::memory_order_relaxed))
                    return;
                const auto rows = provider_->getTableData(table, BATCH_SIZE, offset);
                if (rows.empty()) {
                    spdlog::warn("export of '{}' stopped at row {} of {}", table.name, offset,
                                 total);
                    return;
                }
                for (const auto& row : rows)
                    onRow(row);
                progress_.rowsWritten.fetch_add(static_cast<long long>(rows.size()),
                                                std::memory_order_relaxed);
                progress_.tableRowsWritten.fetch_add(static_cast<long long>(rows.size()),
                                                     std::memory_order_relaxed);
            }
        }

        ITableDataProvider* provider_;
        TableExporter::Progress& progress_;
    };

} // namespace

namespace TableExporter {

    std::optional<Request> chooseDestination(const std::vector<const Table*>& tables,
                                             ExportFormat format, DatabaseType dbType) {
        if (tables.empty())
            return std::nullopt;
        Request request;
        request.format = format;
        request.dbType = dbType;
        for (const Table* t : tables)
            request.tables.push_back(*t);
        // several tables: one sql file, or a folder of one file per table
        if (tables.size() == 1 || format == ExportFormat::SQL)
            request.path = showSaveDialog(format, tables.size() == 1 ? tables[0]->name : "export");
        else
            request.path = showFolderDialog();
        if (request.path.empty())
            return std::nullopt;
        return request;
    }

    Result run(ITableDataProvider* provider, const Request& request, Progress& progress) {
        Result result;
        result.path = request.path;
        if (!provider || request.tables.empty()) {
            result.error = "Nothing to export";
            return result;
        }
        progress.tablesTotal.store(static_cast<int>(request.tables.size()));
        const auto builder = createSQLBuilder(request.dbType);
        TableWriter writer(provider, progress);

        auto writeFile = [&](const std::string& path, const std::vector<Table>& tables) {
            std::ofstream file(path);
            if (!file.is_open()) {
                result.error = std::format("Could not open '{}' for writing", path);
                return false;
            }
            for (size_t i = 0; i < tables.size(); ++i) {
                if (progress.cancelRequested.load(std::memory_order_relaxed))
                    return false;
                if (i > 0)
                    file << "\n";
                if (!writer.write(file, tables[i], request.format, builder.get(), result.error))
                    return false;
                if (progress.cancelRequested.load(std::memory_order_relaxed))
                    return false;
                ++result.tables;
                progress.tablesDone.fetch_add(1, std::memory_order_relaxed);
            }
            file.flush();
            if (!file.good()) {
                result.error = std::format("Writing '{}' failed (disk full?)", path);
                return false;
            }
            return true;
        };

        bool ok = true;
        if (request.tables.size() == 1 || request.format == ExportFormat::SQL) {
            ok = writeFile(request.path, request.tables);
        } else {
            std::error_code ec;
            std::filesystem::create_directories(request.path, ec);
            if (ec) {
                result.error =
                    std::format("Could not create the folder '{}': {}", request.path, ec.message());
                ok = false;
            }
            const char* ext = extensionOf(request.format);
            for (size_t i = 0; ok && i < request.tables.size(); ++i) {
                const auto& table = request.tables[i];
                const auto path =
                    (std::filesystem::path(request.path) / (table.name + "." + ext)).string();
                ok = writeFile(path, {table});
            }
        }
        result.rows = progress.rowsWritten.load();
        result.cancelled = progress.cancelRequested.load();
        result.success = ok && !result.cancelled;
        if (result.success)
            spdlog::info("Exported {} table(s), {} rows to {}", result.tables, result.rows,
                         result.path);
        return result;
    }

} // namespace TableExporter
