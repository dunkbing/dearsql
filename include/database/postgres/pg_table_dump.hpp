#pragma once

#include "database/db.hpp"
#include <libpq-fe.h>
#include <string>

namespace PgTableDump {

    // generate CREATE TABLE DDL from table metadata and schema name
    std::string generateCreateTable(const Table& table, const std::string& schemaName);

    // dump a single table (DDL + data) to a .sql file using COPY protocol
    // conn must be an active libpq connection to the correct database
    bool dumpTable(PGconn* conn, const Table& table, const std::string& schemaName,
                   const std::string& outputPath);

    // show save dialog and dump table
    bool dumpTableInteractive(PGconn* conn, const Table& table, const std::string& schemaName);

} // namespace PgTableDump
