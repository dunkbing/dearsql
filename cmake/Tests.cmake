add_executable(
    database_tests
    tests/database/async_helper_test.cpp
    tests/database/sqlite_database_test.cpp
    tests/database/postgres_database_test.cpp
    tests/database/mysql_database_test.cpp
    tests/database/redis_database_test.cpp
    tests/database/mongodb_database_test.cpp
    tests/database/mssql_database_test.cpp
    tests/database/oracle_database_test.cpp
    tests/database/cassandra_database_test.cpp
    tests/database/ssl_connection_test.cpp
    tests/database/ssh_tunnel_test.cpp
    tests/database/sql_builder_test.cpp
    tests/database/connection_url_test.cpp
    src/database/db_factory.cpp
    src/database/connection_url.cpp
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/postgres/postgres_database_node.cpp
    src/database/postgres/postgres_schema_node.cpp
    src/database/mysql.cpp
    src/database/mysql/mysql_database_node.cpp
    src/database/mysql/mysql_internal.cpp
    src/database/redis.cpp
    src/database/mongodb.cpp
    src/database/mongodb/mongodb_database_node.cpp
    src/database/mssql.cpp
    src/database/mssql/mssql_database_node.cpp
    src/database/mssql/mssql_schema_node.cpp
    src/database/oracle.cpp
    src/database/oracle/oracle_database_node.cpp
    src/database/oracle/oracle_client_installer.cpp
    src/database/cassandra.cpp
    src/database/cassandra/cassandra_database_node.cpp
    src/database/db_utils.cpp
    src/database/sql_builder.cpp
    src/database/ssh_config_parser.cpp
)

if(WIN32)
  target_sources(database_tests PRIVATE src/platform/windows_ssh_tunnel.cpp)
else()
  target_sources(database_tests PRIVATE src/platform/posix_ssh_tunnel.cpp)
endif()

target_include_directories(database_tests PRIVATE include tests/database)
if(SYBDB_INCLUDE_DIR AND NOT SYBDB_INCLUDE_DIR STREQUAL "")
  target_include_directories(
        database_tests
        SYSTEM
        AFTER
        PRIVATE ${SYBDB_INCLUDE_DIR}
    )
endif()

target_link_libraries(
    database_tests
    PRIVATE
        GTest::gtest_main
        unofficial::sqlite3::sqlite3
        PostgreSQL::PostgreSQL
        unofficial::libmariadb
        hiredis::hiredis
        hiredis::hiredis_ssl
        $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
        $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>
        ${SYBDB_LIBRARY}
        ${SYBDB_DEPS}
        odpi
        cassandra_static
        spdlog::spdlog
)

add_test(NAME database_tests COMMAND database_tests)

add_executable(
    sql_format_tests
    tests/ui/sql_format_test.cpp
    tests/ui/csv_parser_test.cpp
    tests/utils/sql_dump_splitter_test.cpp
    src/ui/text_editor_format.cpp
    src/utils/sql_dump_splitter.cpp
)

target_include_directories(
    sql_format_tests
    PRIVATE include external/imgui external/csv2/include
)

target_link_libraries(
    sql_format_tests
    PRIVATE
        GTest::gtest_main
        unofficial::tree-sitter::tree-sitter
        tree-sitter-sql-grammar
)

add_test(NAME sql_format_tests COMMAND sql_format_tests)
