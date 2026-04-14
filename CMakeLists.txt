cmake_minimum_required(VERSION 3.23)

# vcpkg toolchain - must be set before project() call
if(DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
          CACHE STRING "vcpkg toolchain")
else()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake"
          CACHE STRING "vcpkg toolchain")
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  if(APPLE)
    execute_process(
            COMMAND uname -m
            OUTPUT_VARIABLE ARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    if(ARCH STREQUAL "arm64")
      set(VCPKG_TARGET_TRIPLET
                "arm64-osx"
                CACHE STRING
                "vcpkg triplet"
                FORCE
            )
    else()
      set(VCPKG_TARGET_TRIPLET
                "x64-osx"
                CACHE STRING
                "vcpkg triplet"
                FORCE
            )
    endif()
  elseif(UNIX)
    execute_process(
            COMMAND uname -m
            OUTPUT_VARIABLE ARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    if(ARCH STREQUAL "x86_64")
      set(VCPKG_TARGET_TRIPLET
                "x64-linux"
                CACHE STRING
                "vcpkg triplet"
                FORCE
            )
    else()
      set(VCPKG_TARGET_TRIPLET
                "arm64-linux"
                CACHE STRING
                "vcpkg triplet"
                FORCE
            )
    endif()
  elseif(WIN32)
    set(VCPKG_TARGET_TRIPLET
            "x64-windows-static"
            CACHE STRING
            "vcpkg triplet"
            FORCE
        )
  endif()
endif()

if(APPLE)
  set(CMAKE_OSX_DEPLOYMENT_TARGET
        "14.0"
        CACHE STRING
        "Minimum macOS version"
        FORCE
    )
endif()

set(VCPKG_OVERLAY_TRIPLETS
    "${CMAKE_SOURCE_DIR}/cmake/triplets"
    CACHE STRING
    ""
    FORCE
)

project(dearsql)

if(UNIX AND NOT APPLE)
  set(LINUX TRUE)
endif()

# match vcpkg x64-windows-static triplet
if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING
        ""
        FORCE
    )
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
  # jthread, stop_token require experimental libc++ on Apple
  add_compile_options(-fexperimental-library)
  add_link_options(-fexperimental-library)
endif()

file(STRINGS "${CMAKE_SOURCE_DIR}/APP_METADATA" _metadata_lines)
foreach(_line IN LISTS _metadata_lines)
  if(_line MATCHES "^([A-Z_]+)=(.+)$")
    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()

# extract APP_VERSION from the current git tag
execute_process(
  COMMAND git describe --tags --abbrev=0
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE _git_tag
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
string(REGEX REPLACE "^v" "" APP_VERSION "${_git_tag}")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(SENTRY_ENVIRONMENT "production")
else()
  set(SENTRY_ENVIRONMENT "development")
endif()

configure_file(
    ${CMAKE_SOURCE_DIR}/include/config.hpp.in
    ${CMAKE_BINARY_DIR}/include/config.hpp
    @ONLY
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-libmariadb CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(hiredis_ssl CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)
find_package(sentry CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
include(cmake/FreeTDS.cmake)
include(cmake/OracleOCI.cmake)

enable_testing()

if(LINUX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK4 REQUIRED gtk4)
  pkg_check_modules(EPOXY REQUIRED epoxy)
  find_package(X11 REQUIRED)
else()
  find_package(glfw3 CONFIG REQUIRED)
endif()

if(APPLE)
  include(cmake/Sparkle.cmake)
endif()

if(WIN32)
  include(cmake/WinSparkle.cmake)
endif()

include(cmake/TreeSitterGrammars.cmake)
include(cmake/HsqlParser.cmake)
include(cmake/Imgui.cmake)

set(APP_SOURCES
    src/main.cpp
    src/app_state.cpp
    src/embedded_images.cpp
    src/embedded_fonts.cpp
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/postgres/postgres_database_node.cpp
    src/database/postgres/postgres_schema_node.cpp
    src/database/mysql.cpp
    src/database/mysql/mysql_database_node.cpp
    src/database/redis.cpp
    src/database/mongodb.cpp
    src/database/mongodb/mongodb_database_node.cpp
    src/database/mssql.cpp
    src/database/mssql/mssql_database_node.cpp
    src/database/mssql/mssql_schema_node.cpp
    src/database/oracle.cpp
    src/database/oracle/oracle_database_node.cpp
    src/database/oracle/oracle_client_installer.cpp
    src/database/db_factory.cpp
    src/database/db_utils.cpp
    src/database/sql_builder.cpp
    src/database/ssh_config_parser.cpp
    src/ui/query_history.cpp
    src/ui/table_renderer.cpp
    src/ui/datetime_picker.cpp
    src/ui/auto_complete_input.cpp
    src/ui/database_node.cpp
    src/ui/db_sidebar.cpp
    src/ui/tab_manager.cpp
    src/ui/tab/tab.cpp
    src/ui/tab/sql_editor_tab.cpp
    src/ui/tab/table_viewer_tab.cpp
    src/ui/tab/table_editor_tab.cpp
    src/ui/tab/diagram_tab.cpp
    src/ui/text_editor.cpp
    src/ui/text_editor_input.cpp
    src/ui/text_editor_highlight.cpp
    src/ui/text_editor_format.cpp
    src/ui/tab/csv_editor_tab.cpp
    src/ui/tab/routine_viewer_tab.cpp
    src/ui/tab/redis_editor_tab.cpp
    src/ui/tab/redis_key_viewer_tab.cpp
    src/ui/tab/redis_pubsub_tab.cpp
    src/ui/tab/redis_status_panel.cpp
    src/ui/tab/mongo_editor_tab.cpp
    src/utils/file_dialog.cpp
    src/utils/table_exporter.cpp
    src/utils/table_importer.cpp
    src/utils/toggle_button.cpp
    src/utils/spinner.cpp
    src/utils/crypto.cpp
    src/utils/sentry_init.cpp
    src/utils/texture_manager.cpp
    src/ai/ai_client.cpp
    src/ai/ai_chat.cpp
    src/ui/ai_chat_panel.cpp
    src/ui/ai_settings_dialog.cpp
    src/license/license_manager.cpp
)

list(APPEND APP_SOURCES src/application.cpp)
if(APPLE)
  list(APPEND APP_SOURCES src/platform/macos_platform.mm)
  list(APPEND APP_SOURCES src/platform/metal_backend.mm)
  list(APPEND APP_SOURCES src/platform/macos_titlebar.mm)
  list(APPEND APP_SOURCES src/platform/macos_connection_dialog.mm)
  list(APPEND APP_SOURCES src/platform/macos_updater.mm)
  list(APPEND APP_SOURCES src/platform/macos_alert.mm)
  list(APPEND APP_SOURCES src/platform/macos_input_dialog.mm)
  list(APPEND APP_SOURCES src/platform/posix_ssh_tunnel.cpp)
elseif(LINUX)
  list(APPEND APP_SOURCES src/platform/linux_platform.cpp)
  list(APPEND APP_SOURCES src/platform/opengl_backend.cpp)
  list(APPEND APP_SOURCES src/platform/linux_titlebar.cpp)
  list(APPEND APP_SOURCES src/platform/linux_connection_dialog.cpp)
  list(APPEND APP_SOURCES src/platform/linux_updater.cpp)
  list(APPEND APP_SOURCES src/platform/linux_alert.cpp)
  list(APPEND APP_SOURCES src/platform/linux_input_dialog.cpp)
  list(APPEND APP_SOURCES src/platform/posix_ssh_tunnel.cpp)
elseif(WIN32)
  list(
        APPEND APP_SOURCES
        src/platform/windows_platform.cpp
        src/platform/dx11_backend.cpp
        src/platform/windows_titlebar.cpp
        src/platform/windows_connection_dialog.cpp
        src/platform/windows_updater.cpp
        src/platform/windows_alert.cpp
        src/platform/windows_input_dialog.cpp
        src/platform/windows_ssh_tunnel.cpp
        assets/app.rc
    )
endif()

include(cmake/EmbedResources.cmake)
embed_fonts()
embed_images()

list(APPEND APP_SOURCES ${EMBEDDED_FONTS_FILE} ${EMBEDDED_IMAGES_FILE})

add_executable(
    ${PROJECT_NAME}
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUI_NODE_EDITOR_SOURCES}
)

# hide console on release, keep main() entry point
if(WIN32)
  target_link_options(
        ${PROJECT_NAME}
        PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
            $<$<CONFIG:Release>:/ENTRY:mainCRTStartup>
    )
endif()

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        include
        external/imgui
        external/imgui/backends
        external/imgui-node-editor
        external/imgui-node-editor/external/stb_image
        external/IconFontCppHeaders
        external/csv2/include
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/include
)

if(WIN32)
  target_compile_definitions(
        ${PROJECT_NAME}
        PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    )
  # Common Controls v6 manifest (needed for TaskDialogIndirect)
  set_property(
        TARGET ${PROJECT_NAME}
        APPEND
        PROPERTY
            LINK_FLAGS
                "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/assets/app.manifest"
    )
endif()

if(LINUX)
  target_include_directories(
        ${PROJECT_NAME}
        PRIVATE ${GTK4_INCLUDE_DIRS} ${EPOXY_INCLUDE_DIRS}
    )
endif()
# SYSTEM to avoid shadowing vcpkg MySQL/OpenSSL headers
if(SYBDB_INCLUDE_DIR AND NOT SYBDB_INCLUDE_DIR STREQUAL "")
  target_include_directories(
        ${PROJECT_NAME}
        SYSTEM
        AFTER
        PRIVATE ${SYBDB_INCLUDE_DIR}
    )
endif()

if(APPLE)
  target_include_directories(
        ${PROJECT_NAME}
        PRIVATE "${SPARKLE_FRAMEWORK_PATH}/Headers"
    )
endif()

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        unofficial::sqlite3::sqlite3
        nlohmann_json::nlohmann_json
        PostgreSQL::PostgreSQL
        unofficial::libmariadb
        hiredis::hiredis
        hiredis::hiredis_ssl
        $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
        $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>
        nfd::nfd
        OpenSSL::SSL
        OpenSSL::Crypto
        sentry::sentry
        ${SYBDB_LIBRARY}
        ${SYBDB_DEPS}
        odpi
        httplib::httplib
        unofficial::tree-sitter::tree-sitter
        tree-sitter-sql-grammar
        tree-sitter-json-grammar
        spdlog::spdlog
        sql-parser
)

if(APPLE)
  target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE
            glfw
            ${METAL_LIBRARY}
            ${QUARTZCORE_LIBRARY}
            ${FOUNDATION_LIBRARY}
            ${APPKIT_LIBRARY}
    )
  target_link_libraries(${PROJECT_NAME} PRIVATE "${SPARKLE_FRAMEWORK_PATH}")
elseif(LINUX)
  target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE ${GTK4_LIBRARIES} ${EPOXY_LIBRARIES} ${X11_LIBRARIES}
    )
  target_link_directories(
        ${PROJECT_NAME}
        PRIVATE ${GTK4_LIBRARY_DIRS} ${EPOXY_LIBRARY_DIRS}
    )
elseif(WIN32)
  target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE glfw d3d11 dxgi dwmapi comctl32 uxtheme ws2_32 WinSparkle
    )
  add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different "${WINSPARKLE_DLL}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        COMMENT "Copying WinSparkle.dll to output directory"
    )
else()
  target_link_libraries(${PROJECT_NAME} PRIVATE glfw OpenGL::GL)
endif()

if(APPLE)
  configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
        "${CMAKE_BINARY_DIR}/Info.plist.in"
        @ONLY
    )
  set_target_properties(
        ${PROJECT_NAME}
        PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/Info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_IDENTIFIER}"
            MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}"
            MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
            OUTPUT_NAME "${APP_NAME}"
            BUILD_RPATH "@loader_path/../Frameworks"
            INSTALL_RPATH "@loader_path/../Frameworks"
    )

  set_source_files_properties(
        assets/AppIcon.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
  target_sources(${PROJECT_NAME} PRIVATE assets/AppIcon.icns)

  set(_SPARKLE_DEST
        "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Frameworks/Sparkle.framework"
    )
  add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_SPARKLE_DEST}"
        COMMAND ditto "${SPARKLE_FRAMEWORK_PATH}" "${_SPARKLE_DEST}"
        # Fix Versions/Current: must be a symlink to B, not a directory copy
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_SPARKLE_DEST}/Versions/Current"
        COMMAND ln -s B "${_SPARKLE_DEST}/Versions/Current"
        COMMENT "Embedding Sparkle.framework into app bundle"
    )

  add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:sentry::sentry>"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Frameworks/"
        COMMENT "Embedding sentry dylib into app bundle"
    )
endif()

include(cmake/Tests.cmake)

if(LINUX)
  install(TARGETS ${PROJECT_NAME} DESTINATION bin)
  install(
        FILES packaging/io.gitlab.dunkbing.dearsql.desktop
        DESTINATION share/applications
    )
  install(
        FILES packaging/io.gitlab.dunkbing.dearsql.metainfo.xml
        DESTINATION share/metainfo
    )
  install(
        FILES assets/appicon.png
        DESTINATION share/icons/hicolor/256x256/apps
        RENAME io.gitlab.dunkbing.dearsql.png
    )
endif()
