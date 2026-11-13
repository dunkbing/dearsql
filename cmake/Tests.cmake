add_executable(
    database_tests
    tests/database/async_helper_test.cpp
    tests/database/sqlite_database_test.cpp
    tests/database/duckdb_database_test.cpp
    tests/database/postgres_database_test.cpp
    tests/database/mysql_database_test.cpp
    tests/database/redis_database_test.cpp
    tests/database/s3_test.cpp
    tests/database/mongodb_database_test.cpp
    tests/database/mssql_database_test.cpp
    tests/database/oracle_database_test.cpp
    tests/database/cassandra_database_test.cpp
    tests/database/ssl_connection_test.cpp
    tests/database/ssh_tunnel_test.cpp
    tests/database/sql_builder_test.cpp
    tests/database/connection_url_test.cpp
    tests/database/mcp_tools_test.cpp
    tests/database/cli_command_test.cpp
    tests/database/process_runner_test.cpp
    tests/database/lsp_test.cpp
    tests/database/table_transfer_test.cpp
    src/cli/completion_catalog.cpp
    src/cli/connections.cpp
    src/cli/lsp.cpp
    src/utils/cli_command.cpp
    src/utils/process_runner.cpp
    src/mcp/db_tools.cpp
    src/database/db_factory.cpp
    src/database/connection_url.cpp
    src/database/file_database.cpp
    src/database/postgres/postgres_database_node.cpp
    src/database/mysql/mysql_database_node.cpp
    src/database/redis.cpp
    src/database/s3.cpp
    src/database/s3_client.cpp
    src/database/mongodb/mongodb_database_node.cpp
    src/database/mssql/mssql_database_node.cpp
    src/database/oracle.cpp
    src/database/oracle/oracle_database_node.cpp
    src/database/oracle/oracle_client_installer.cpp
    src/database/cassandra/cassandra_database_node.cpp
    src/database/db_utils.cpp
    src/database/lib_database_node.cpp
    src/database/ssh_config_parser.cpp
    src/utils/table_exporter.cpp
    src/utils/table_importer.cpp
    src/utils/file_dialog.cpp
    tests/database/credential_migration_test.cpp
    src/app_state.cpp
    src/utils/crypto.cpp
    src/utils/master_secret.cpp
)

target_sources(database_tests PRIVATE src/database/ssh_tunnel.cpp)

target_include_directories(
    database_tests
    PRIVATE include tests/database ${CMAKE_BINARY_DIR}/include ${IMSHELL_IMGUI_DIR}
)

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
        dearsql::dearsql
        imshell::imshell
        miniocpp::miniocpp
        libssh2::libssh2
        spdlog::spdlog
        OpenSSL::SSL
        OpenSSL::Crypto
        nfd::nfd
)

# master secret keystore backends
if(APPLE)
  target_sources(database_tests PRIVATE src/utils/synced_store.mm)
  target_link_libraries(database_tests PRIVATE ${SECURITY_LIBRARY} "-framework CoreData")
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
    tests/ui/csv_document_test.cpp
    tests/ui/chart_data_test.cpp
    tests/ui/env_tag_test.cpp
    tests/ui/themes_test.cpp
    tests/utils/mysql_dump_splitter_test.cpp
    tests/utils/sql_guard_test.cpp
    external/libdearsql/tests/completion_tests.cpp
    external/libdearsql/tests/delete_impact_tests.cpp
    external/libdearsql/tests/explain_tests.cpp
    external/libdearsql/tests/schema_tools_tests.cpp
    external/libdearsql/tests/syntax_tests.cpp
    src/ui/text_editor_format.cpp
    src/utils/mysql_dump_splitter.cpp
    src/utils/csv_document.cpp
    src/utils/chart_data.cpp
    ${IMGUI_SOURCES} # imshell's themes.cpp, for the theme test
)

target_include_directories(
    sql_format_tests
    PRIVATE include ${IMSHELL_IMGUI_DIR}
)

target_link_libraries(
    sql_format_tests
    PRIVATE
        GTest::gtest_main
        dearsql::dearsql
        imshell::imshell
        unofficial::tree-sitter::tree-sitter
        tree-sitter-sql-grammar
        nlohmann_json::nlohmann_json
)

add_test(NAME sql_format_tests COMMAND sql_format_tests)

# ---------------------------------------------------------------------------
# ui_tests: drives the real app through Dear ImGui's test engine.
# Links the whole application (minus its main) so tests can click actual
# widgets by id instead of poking at internals.
# ---------------------------------------------------------------------------
set(IMGUI_TE_DIR ${IMSHELL_TEST_ENGINE_DIR})

if(APPLE AND EXISTS ${IMGUI_TE_DIR}/imgui_te_engine.cpp)
    set(UI_TEST_APP_SOURCES ${APP_SOURCES})
    list(REMOVE_ITEM UI_TEST_APP_SOURCES src/main.cpp)

    add_executable(
        ui_tests
        ${UI_TEST_APP_SOURCES}
        ${IMGUI_SOURCES}
        ${IMGUI_TE_DIR}/imgui_te_context.cpp
        ${IMGUI_TE_DIR}/imgui_te_coroutine.cpp
        ${IMGUI_TE_DIR}/imgui_te_engine.cpp
        ${IMGUI_TE_DIR}/imgui_te_exporters.cpp
        ${IMGUI_TE_DIR}/imgui_te_perftool.cpp
        ${IMGUI_TE_DIR}/imgui_te_ui.cpp
        ${IMGUI_TE_DIR}/imgui_te_utils.cpp
        ${IMGUI_TE_DIR}/imgui_capture_tool.cpp
        tests/ui/main_test.mm
        tests/ui/frame_capture.mm
        tests/ui/sidebar_tests.cpp
        tests/ui/ai_panel_tests.cpp
        tests/ui/table_viewer_tests.cpp
        tests/ui/csv_editor_tests.cpp
        tests/ui/docs_shots.cpp
    )

    # its metal objects are owned by arc
    set_source_files_properties(tests/ui/frame_capture.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)

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
