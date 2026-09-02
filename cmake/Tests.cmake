add_executable(
    database_tests
    tests/database/async_helper_test.cpp
    tests/database/sqlite_database_test.cpp
    tests/database/duckdb_database_test.cpp
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
    src/database/duckdb.cpp
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
    tests/database/credential_migration_test.cpp
    src/app_state.cpp
    src/utils/crypto.cpp
    src/utils/master_secret.cpp
)

if(WIN32)
  target_sources(database_tests PRIVATE src/platform/windows_ssh_tunnel.cpp)
else()
  target_sources(database_tests PRIVATE src/platform/posix_ssh_tunnel.cpp)
endif()

target_include_directories(
    database_tests
    PRIVATE include tests/database ${CMAKE_BINARY_DIR}/include
)
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
        $<IF:$<TARGET_EXISTS:duckdb>,duckdb,duckdb_static>
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
        OpenSSL::SSL
        OpenSSL::Crypto
)

# master secret keystore backends
if(APPLE)
  target_link_libraries(database_tests PRIVATE ${SECURITY_LIBRARY})
elseif(WIN32)
  target_link_libraries(database_tests PRIVATE advapi32)
elseif(LINUX AND LIBSECRET_FOUND)
  target_compile_definitions(database_tests PRIVATE HAVE_LIBSECRET)
  target_include_directories(database_tests PRIVATE ${LIBSECRET_INCLUDE_DIRS})
  target_link_libraries(database_tests PRIVATE ${LIBSECRET_LIBRARIES})
  target_link_directories(database_tests PRIVATE ${LIBSECRET_LIBRARY_DIRS})
endif()

add_test(NAME database_tests COMMAND database_tests)

add_executable(
    sql_format_tests
    tests/ui/sql_format_test.cpp
    tests/ui/csv_parser_test.cpp
    tests/utils/mysql_dump_splitter_test.cpp
    tests/utils/sql_guard_test.cpp
    src/ui/text_editor_format.cpp
    src/utils/mysql_dump_splitter.cpp
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

# ---------------------------------------------------------------------------
# ui_tests: drives the real app through Dear ImGui's test engine.
# Links the whole application (minus its main) so tests can click actual
# widgets by id instead of poking at internals.
# ---------------------------------------------------------------------------
set(IMGUI_TE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui_test_engine/imgui_test_engine)

if(APPLE AND EXISTS ${IMGUI_TE_DIR}/imgui_te_engine.cpp)
    set(UI_TEST_APP_SOURCES ${APP_SOURCES})
    list(REMOVE_ITEM UI_TEST_APP_SOURCES src/main.cpp)

    add_executable(
        ui_tests
        ${UI_TEST_APP_SOURCES}
        ${IMGUI_SOURCES}
        ${IMGUI_NODE_EDITOR_SOURCES}
        ${IMGUI_TE_DIR}/imgui_te_context.cpp
        ${IMGUI_TE_DIR}/imgui_te_coroutine.cpp
        ${IMGUI_TE_DIR}/imgui_te_engine.cpp
        ${IMGUI_TE_DIR}/imgui_te_exporters.cpp
        ${IMGUI_TE_DIR}/imgui_te_perftool.cpp
        ${IMGUI_TE_DIR}/imgui_te_ui.cpp
        ${IMGUI_TE_DIR}/imgui_te_utils.cpp
        ${IMGUI_TE_DIR}/imgui_capture_tool.cpp
        tests/ui/main_test.mm
        tests/ui/sidebar_tests.cpp
        tests/ui/ai_panel_tests.cpp
    )

    # the engine hooks into imgui through this define; both are compiled here
    target_compile_definitions(
        ui_tests
        PRIVATE IMGUI_ENABLE_TEST_ENGINE IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
    )

    # mirror the app target rather than restating its dependency list
    get_target_property(_app_incs ${PROJECT_NAME} INCLUDE_DIRECTORIES)
    get_target_property(_app_libs ${PROJECT_NAME} LINK_LIBRARIES)
    get_target_property(_app_defs ${PROJECT_NAME} COMPILE_DEFINITIONS)
    if(_app_incs)
        target_include_directories(ui_tests PRIVATE ${_app_incs})
    endif()
    if(_app_libs)
        target_link_libraries(ui_tests PRIVATE ${_app_libs})
    endif()
    if(_app_defs)
        target_compile_definitions(ui_tests PRIVATE ${_app_defs})
    endif()
    # headers sit in the inner imgui_test_engine/ folder
    target_include_directories(ui_tests PRIVATE ${IMGUI_TE_DIR})

    add_test(NAME ui_tests COMMAND ui_tests -nopause)
endif()
