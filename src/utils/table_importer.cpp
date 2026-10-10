#include "utils/table_importer.hpp"
#include "database/ddl_utils.hpp"
#include "database/sql_builder.hpp"
#include <filesystem>
#include <format>
#include <fstream>
#include <nfd.h>
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
        case DatabaseType::MYSQL:
        case DatabaseType::MARIADB:
        case DatabaseType::MSSQL:
            return true;
        default:
            return false;
        }
    }

    // sql server caps a VALUES list at 1000 rows
    constexpr size_t BATCH_ROWS = 500;

} // namespace

namespace TableImporter {

    std::optional<std::string> chooseCsvFile() {
        nfdfilteritem_t filter = {"CSV Files", "csv"};
        nfdchar_t* outPath = nullptr;
        const nfdresult_t result = NFD_OpenDialog(&outPath, &filter, 1, nullptr);
        if (result != NFD_OKAY) {
            if (result == NFD_ERROR)
                spdlog::error("File dialog error: {}", NFD_GetError());
            return std::nullopt;
        }
        std::string path(outPath);
        NFD_FreePath(outPath);
        return path;
    }

    Result importCsv(IDatabaseNode* node, const std::string& tableName, const std::string& path,
                     Progress& progress) {
        Result result;
        result.path = path;
        if (!node) {
            result.error = "No database";
            return result;
        }
        const auto builder = createSQLBuilder(node->getDatabaseType());
        const std::string quotedTable = builder->quoteIdentifier(tableName);
        const bool multiRow = supportsMultiRowInsert(node->getDatabaseType());

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

        auto literalsOf = [&](const std::string& line) {
            const auto values = parseLine(line);
            std::vector<std::string> literals;
            literals.reserve(columns.size());
            for (size_t i = 0; i < columns.size(); ++i) {
                const std::string& val = i < values.size() ? values[i] : "";
                literals.push_back(val.empty() ? "NULL"
                                               : "'" + ddl_utils::escapeSingleQuotes(val) + "'");
            }
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
            spdlog::error("CSV import row failed: {}", error);
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
        // one statement for the batch; on failure each row on its own, so a bad row
        // costs only itself
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
        std::string line;
        long long bytes = static_cast<long long>(headerLine.size()) + 1;
        while (std::getline(file, line)) {
            if (progress.cancelRequested.load(std::memory_order_relaxed))
                break;
            bytes += static_cast<long long>(line.size()) + 1;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            batch.push_back(literalsOf(line));
            if (batch.size() >= BATCH_ROWS) {
                flush(batch);
                progress.bytesRead.store(bytes, std::memory_order_relaxed);
            }
        }
        if (!progress.cancelRequested.load(std::memory_order_relaxed))
            flush(batch);
        progress.bytesRead.store(bytes, std::memory_order_relaxed);

        result.cancelled = progress.cancelRequested.load();
        result.success = !result.cancelled && result.failed == 0;
        spdlog::info("CSV import of {}: {} inserted, {} failed{}", path, result.inserted,
                     result.failed, result.cancelled ? " (cancelled)" : "");
        return result;
    }

} // namespace TableImporter
