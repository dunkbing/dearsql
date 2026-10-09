#pragma once

#include "db.hpp"
#include "db_interface.hpp"
#include <dearsql/sql_builder.hpp>

// dialect builders live in libdearsql
using dearsql::CassandraBuilder;
using dearsql::createSQLBuilder;
using dearsql::DuckDBBuilder;
using dearsql::ISQLBuilder;
using dearsql::MSSQLBuilder;
using dearsql::MySQLBuilder;
using dearsql::OracleBuilder;
using dearsql::PostgreSQLBuilder;
using dearsql::SQLiteBuilder;
