#include "database/postgres/pg_table_dump.hpp"
#include <format>
#include <fstream>
#include <memory>
#include <nfd.h>
#include <spdlog/spdlog.h>

namespace {

    struct PgResultDeleter {
        void operator()(PGresult* r) const {
            if (r)
                PQclear(r);
        }
    };
    using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

    std::string quotePgIdentifier(const std::string& id) {
        std::string out = "\"";
        for (char ch : id) {
            out += ch;
            if (ch == '"')
                out += '"';
        }
        out += '"';
        return out;
    }

    std::string qualifiedName(const std::string& schema, const std::string& table) {
        return quotePgIdentifier(schema) + "." + quotePgIdentifier(table);
    }

    std::string showDumpSaveDialog(const std::string& tableName) {
        nfdfilteritem_t filter = {"SQL Files", "sql"};
        std::string defaultName = tableName + ".sql";

        nfdchar_t* outPath = nullptr;
        nfdresult_t result = NFD_SaveDialog(&outPath, &filter, 1, nullptr, defaultName.c_str());
        if (result == NFD_OKAY) {
            std::string path(outPath);
            NFD_FreePath(outPath);
            return path;
        }
        if (result == NFD_ERROR) {
            spdlog::error("File dialog error: {}", NFD_GetError());
        }
        return "";
    }

} // namespace

namespace PgTableDump {

    std::string generateCreateTable(const Table& table, const std::string& schemaName) {
        std::string ddl =
            std::format("CREATE TABLE {} (\n", qualifiedName(schemaName, table.name));

        // columns
        for (size_t i = 0; i < table.columns.size(); ++i) {
            const auto& col = table.columns[i];
            ddl += "    " + quotePgIdentifier(col.name) + " " + col.type;
            if (col.isNotNull)
                ddl += " NOT NULL";
            if (!col.defaultValue.empty())
                ddl += " DEFAULT " + col.defaultValue;
            if (i + 1 < table.columns.size() || !table.indexes.empty() || !table.foreignKeys.empty())
                ddl += ",";
            ddl += "\n";
        }

        // primary key constraint
        for (const auto& idx : table.indexes) {
            if (!idx.isPrimary)
                continue;
            ddl += "    CONSTRAINT " + quotePgIdentifier(idx.name) + " PRIMARY KEY (";
            for (size_t i = 0; i < idx.columns.size(); ++i) {
                if (i > 0)
                    ddl += ", ";
                ddl += quotePgIdentifier(idx.columns[i]);
            }
            ddl += ")";
            if (!table.foreignKeys.empty())
                ddl += ",";
            ddl += "\n";
            break;
        }

        // foreign keys
        for (size_t i = 0; i < table.foreignKeys.size(); ++i) {
            const auto& fk = table.foreignKeys[i];
            ddl += "    CONSTRAINT " + quotePgIdentifier(fk.name) + " FOREIGN KEY (" +
                   quotePgIdentifier(fk.sourceColumn) + ") REFERENCES " +
                   quotePgIdentifier(fk.targetTable) + " (" + quotePgIdentifier(fk.targetColumn) +
                   ")";
            if (!fk.onDelete.empty() && fk.onDelete != "NO ACTION")
                ddl += " ON DELETE " + fk.onDelete;
            if (!fk.onUpdate.empty() && fk.onUpdate != "NO ACTION")
                ddl += " ON UPDATE " + fk.onUpdate;
            if (i + 1 < table.foreignKeys.size())
                ddl += ",";
            ddl += "\n";
        }

        ddl += ");\n";

        // non-primary indexes
        for (const auto& idx : table.indexes) {
            if (idx.isPrimary)
                continue;
            ddl += std::format("CREATE {}INDEX {} ON {} ", (idx.isUnique ? "UNIQUE " : ""),
                               quotePgIdentifier(idx.name), qualifiedName(schemaName, table.name));
            if (!idx.type.empty() && idx.type != "BTREE" && idx.type != "btree")
                ddl += "USING " + idx.type + " ";
            ddl += "(";
            for (size_t i = 0; i < idx.columns.size(); ++i) {
                if (i > 0)
                    ddl += ", ";
                ddl += quotePgIdentifier(idx.columns[i]);
            }
            ddl += ");\n";
        }

        return ddl;
    }

    bool dumpTable(PGconn* conn, const Table& table, const std::string& schemaName,
                   const std::string& outputPath) {
        std::ofstream file(outputPath);
        if (!file.is_open()) {
            spdlog::error("Failed to open file for writing: {}", outputPath);
            return false;
        }

        file << "-- dump of table " << schemaName << "." << table.name << "\n\n";

        // DDL
        file << generateCreateTable(table, schemaName) << "\n";

        // get column names for COPY header
        std::vector<std::string> colNames;
        {
            std::string query = std::format(
                "SELECT attname FROM pg_catalog.pg_attribute a "
                "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
                "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
                "WHERE n.nspname = '{}' AND c.relname = '{}' "
                "AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum",
                schemaName, table.name);
            PgResultPtr res(PQexec(conn, query.c_str()));
            if (res && PQresultStatus(res.get()) == PGRES_TUPLES_OK) {
                int n = PQntuples(res.get());
                for (int i = 0; i < n; ++i)
                    colNames.emplace_back(PQgetvalue(res.get(), i, 0));
            }
        }

        if (colNames.empty()) {
            spdlog::warn("No columns found for {}.{}, skipping data", schemaName, table.name);
            file.close();
            return true;
        }

        // COPY header
        file << "COPY " << qualifiedName(schemaName, table.name) << " (";
        for (size_t i = 0; i < colNames.size(); ++i) {
            if (i > 0)
                file << ", ";
            file << quotePgIdentifier(colNames[i]);
        }
        file << ") FROM stdin;\n";

        // stream COPY data
        std::string copyCmd = std::format("COPY {}.{} TO STDOUT", quotePgIdentifier(schemaName),
                                          quotePgIdentifier(table.name));
        PgResultPtr copyRes(PQexec(conn, copyCmd.c_str()));
        if (PQresultStatus(copyRes.get()) != PGRES_COPY_OUT) {
            spdlog::error("COPY failed: {}", PQerrorMessage(conn));
            file << "\\.\n";
            file.close();
            return false;
        }

        char* buffer = nullptr;
        int nbytes;
        while ((nbytes = PQgetCopyData(conn, &buffer, 0)) > 0) {
            file.write(buffer, nbytes);
            PQfreemem(buffer);
            buffer = nullptr;
        }
        if (buffer)
            PQfreemem(buffer);

        // consume final result
        PgResultPtr endRes(PQgetResult(conn));

        if (nbytes == -2) {
            spdlog::error("COPY OUT error: {}", PQerrorMessage(conn));
            file << "\\.\n";
            file.close();
            return false;
        }

        file << "\\.\n";
        file.close();

        spdlog::info("Table {}.{} dumped to {}", schemaName, table.name, outputPath);
        return true;
    }

    bool dumpTableInteractive(PGconn* conn, const Table& table, const std::string& schemaName) {
        std::string path = showDumpSaveDialog(table.name);
        if (path.empty())
            return false;
        return dumpTable(conn, table, schemaName, path);
    }

} // namespace PgTableDump
