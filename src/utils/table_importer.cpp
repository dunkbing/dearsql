#include "utils/table_importer.hpp"
#include "database/ddl_utils.hpp"
#include "database/sql_builder.hpp"
#include <algorithm>
#include <dearsql/factory.hpp>
#include <filesystem>
#include <format>
#include <fstream>
#include <spdlog/spdlog.h>

namespace {

    // one csv line into fields ("" quoting, "" escapes a quote). the old field
    // parser never advanced past the last field and looped forever
    std::vector<std::string> parseLine(const std::string& line) {
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (quoted) {
                if (c != '"')
                    field += c;
                else if (i + 1 < line.size() && line[i + 1] == '"')
                    field += line[++i];
                else
                    quoted = false;
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                fields.push_back(std::move(field));
                field.clear();
            } else {
                field += c;
            }
        }
        fields.push_back(std::move(field));
        return fields;
    }

    // dialects that take INSERT ... VALUES (...), (...)
    bool supportsMultiRowInsert(DatabaseType type) {
        switch (type) {
        case DatabaseType::SQLITE:
        case DatabaseType::DUCKDB:
        case DatabaseType::POSTGRESQL:
        case DatabaseType::REDSHIFT:
        case DatabaseType::COCKROACHDB:
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB:
        case DatabaseType::TIDB:
        case DatabaseType::MSSQL:
            return true;
        default:
            return false;
        }
    }

    // sql server caps a VALUES list at 1000 rows
    constexpr size_t BATCH_ROWS = 500;


    // a value as a sql literal: empty and NULL are NULL, booleans 1/0 (every engine
    // casts those), the rest quoted text the column casts
    std::string literalOf(const std::string& v) {
        if (v.empty() || isNullSentinel(v))
            return "NULL";
        if (isBoolSentinel(v))
            return boolSentinelValue(v) ? "'1'" : "'0'";
        return "'" + ddl_utils::escapeSingleQuotes(v) + "'";
    }

    // rows from next() (false when done) into the table in batches. a failing batch is
    // retried row by row, so a bad row costs only itself. advanced() reports progress
    void importRows(IDatabaseNode* node, const std::string& tableName,
                    const std::vector<std::string>& columns,
                    const std::function<bool(std::vector<std::string>&)>& next,
                    const std::function<void()>& advanced, TableImporter::Progress& progress,
                    TableImporter::Result& result) {
        const auto builder = createSQLBuilder(node->getDatabaseType());
        const std::string quotedTable = builder->quoteIdentifier(tableName);
        const bool multiRow = supportsMultiRowInsert(node->getDatabaseType());
        auto literalsOf = [&](const std::vector<std::string>& values) {
            std::vector<std::string> literals;
            literals.reserve(columns.size());
            for (size_t i = 0; i < columns.size(); ++i)
                literals.push_back(literalOf(i < values.size() ? values[i] : ""));
            return literals;
        };
        auto rowSql = [&](const std::vector<std::string>& literals) {
            return builder->insertRow(quotedTable, columns, literals);
        };
        auto noteFailure = [&](const std::string& error) {
            result.failed++;
            progress.failed.fetch_add(1, std::memory_order_relaxed);
            if (result.error.empty())
                result.error = error;
            spdlog::error("import row failed: {}", error);
        };
        auto insertOne = [&](const std::vector<std::string>& literals) {
            auto r = node->executeQuery(rowSql(literals));
            if (r.success()) {
                result.inserted++;
                progress.inserted.fetch_add(1, std::memory_order_relaxed);
            } else {
                noteFailure(r.errorMessage());
            }
        };
        auto flush = [&](std::vector<std::vector<std::string>>& batch) {
            if (batch.empty())
                return;
            if (!multiRow || batch.size() == 1) {
                for (const auto& literals : batch)
                    insertOne(literals);
            } else {
                std::string sql = rowSql(batch.front());
                for (size_t i = 1; i < batch.size(); ++i) {
                    sql += ", (";
                    for (size_t c = 0; c < batch[i].size(); ++c)
                        sql += (c > 0 ? ", " : "") + batch[i][c];
                    sql += ')';
                }
                auto r = node->executeQuery(sql);
                if (r.success()) {
                    result.inserted += static_cast<long long>(batch.size());
                    progress.inserted.fetch_add(static_cast<long long>(batch.size()),
                                                std::memory_order_relaxed);
                } else {
                    for (const auto& literals : batch) {
                        if (progress.cancelRequested.load(std::memory_order_relaxed))
                            break;
                        insertOne(literals);
                    }
                }
            }
            batch.clear();
        };

        std::vector<std::vector<std::string>> batch;
        batch.reserve(BATCH_ROWS);
        std::vector<std::string> values;
        while (!progress.cancelRequested.load(std::memory_order_relaxed) && next(values)) {
            batch.push_back(literalsOf(values));
            if (batch.size() >= BATCH_ROWS) {
                flush(batch);
                advanced();
            }
        }
        if (!progress.cancelRequested.load(std::memory_order_relaxed))
            flush(batch);
        advanced();
        result.cancelled = progress.cancelRequested.load();
        result.success = !result.cancelled && result.failed == 0;
    }


} // namespace

namespace TableImporter {

    Result importCsv(IDatabaseNode* node, const std::string& tableName, const std::string& path,
                     Progress& progress) {
        Result result;
        result.path = path;
        if (!node) {
            result.error = "No database";
            return result;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            result.error = std::format("Could not open '{}'", path);
            return result;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        progress.totalBytes.store(ec ? 0 : static_cast<long long>(size));

        std::string headerLine;
        if (!std::getline(file, headerLine)) {
            result.error = "The CSV file is empty";
            return result;
        }
        if (!headerLine.empty() && headerLine.back() == '\r')
            headerLine.pop_back();
        const auto columns = parseLine(headerLine);
        if (columns.empty()) {
            result.error = "The CSV header row has no columns";
            return result;
        }

        long long bytes = static_cast<long long>(headerLine.size()) + 1;
        std::string line;
        const auto next = [&](std::vector<std::string>& values) {
            while (std::getline(file, line)) {
                bytes += static_cast<long long>(line.size()) + 1;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                values = parseLine(line);
                return true;
            }
            return false;
        };
        importRows(node, tableName, columns, next,
                   [&] { progress.bytesRead.store(bytes, std::memory_order_relaxed); }, progress,
                   result);
        spdlog::info("CSV import of {}: {} inserted, {} failed{}", path, result.inserted,
                     result.failed, result.cancelled ? " (cancelled)" : "");
        return result;
    }

#ifndef DEARSQL_MOBILE
    Result importXlsx(IDatabaseNode* node, const std::string& tableName, const std::string& path,
                      Progress& progress) {
        Result result;
        result.path = path;
        if (!node) {
            result.error = "No database";
            return result;
        }
        // the workbook through the library's reader: header, types and dates as when opened
        dearsql::ConnectionInfo info;
        info.type = DatabaseType::SQLITE;
        info.path = path;
        info.readOnly = true;
        auto conn = dearsql::makeConnection(info);
        if (!conn) {
            result.error = "This build cannot read Excel workbooks";
            return result;
        }
        if (auto [ok, err] = conn->open(); !ok) {
            result.error = err;
            return result;
        }
        auto book = conn->database();
        std::vector<std::string> sheets;
        try {
            for (const auto& t : book->tables())
                sheets.push_back(t.name);
        } catch (const std::exception& e) {
            result.error = e.what();
            return result;
        }
        if (sheets.empty()) {
            result.error = "The workbook has no sheets";
            return result;
        }
        // shortcut: no sheet picker; the sheet named like the table, else the first. add a
        // picker when users import from multi-sheet workbooks
        std::string sheet = sheets.front();
        for (const auto& s : sheets)
            if (std::ranges::equal(s, tableName, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }))
                sheet = s;
        const auto builder = createSQLBuilder(DatabaseType::SQLITE);
        auto read = book->execute("SELECT * FROM " + builder->quoteIdentifier(sheet), 0);
        if (!read.success() || read.statements.empty()) {
            result.error = read.errorMessage();
            return result;
        }
        const auto& data = read.statements.front();
        if (data.columnNames.empty()) {
            result.error = std::format("Sheet '{}' is empty", sheet);
            return result;
        }
        std::error_code ec;
        const auto size = static_cast<long long>(std::filesystem::file_size(path, ec));
        const auto rows = static_cast<long long>(data.tableData.size());
        progress.totalBytes.store(ec ? 0 : size);
        size_t at = 0;
        const auto next = [&](std::vector<std::string>& values) {
            if (at >= data.tableData.size())
                return false;
            values = data.tableData[at++];
            return true;
        };
        importRows(
            node, tableName, data.columnNames, next,
            [&] {
                if (!ec && rows > 0)
                    progress.bytesRead.store(size * static_cast<long long>(at) / rows,
                                             std::memory_order_relaxed);
            },
            progress, result);
        spdlog::info("Excel import of {} ({}): {} inserted, {} failed{}", path, sheet,
                     result.inserted, result.failed, result.cancelled ? " (cancelled)" : "");
        return result;
    }
#endif

} // namespace TableImporter
