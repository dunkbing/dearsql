cmake_minimum_required(VERSION 3.23)
project(dear-sql)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Application metadata
set(APP_NAME "DearSQL")
set(APP_VERSION "0.1.0")
set(APP_IDENTIFIER "dearsql.db99.dev")

configure_file(
    ${CMAKE_SOURCE_DIR}/include/config.hpp.in
    ${CMAKE_BINARY_DIR}/include/config.hpp
    @ONLY
)

# Dependencies
add_subdirectory(external/json)

# SQLite3 - use embedded amalgamation for portable builds (must be before SOCI)
add_library(sqlite3_static STATIC external/sqlite3/sqlite3.c)
target_include_directories(sqlite3_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/external/sqlite3>
)
target_compile_definitions(sqlite3_static PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_ENABLE_RTREE
)
# Create SQLite::SQLite3 alias that SOCI/CMake expects
add_library(SQLite::SQLite3 ALIAS sqlite3_static)

# MariaDB Connector/C - build as static for portable builds (must be before SOCI)
set(WITH_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_SSL "OPENSSL" CACHE STRING "" FORCE)
set(WITH_CURL OFF CACHE BOOL "" FORCE)
set(WITH_EXTERNAL_ZLIB OFF CACHE BOOL "" FORCE)
# Disable dynamic plugins - use static only
set(CLIENT_PLUGIN_PVIO_SOCKET STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PVIO_NPIPE OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PVIO_SHMEM OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_SHA256_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CACHING_SHA2_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_MYSQL_NATIVE_PASSWORD STATIC CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD STATIC CACHE STRING "" FORCE)
# Disable problematic plugins
set(CLIENT_PLUGIN_DIALOG OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_CLIENT_ED25519 OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_PARSEC OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_ZSTD OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_REMOTE_IO OFF CACHE STRING "" FORCE)
set(CLIENT_PLUGIN_AUTH_GSSAPI_CLIENT OFF CACHE STRING "" FORCE)
set(WITH_GSSAPI OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static mariadb connector" FORCE)
set(INSTALL_PLUGINDIR "" CACHE STRING "" FORCE)
add_subdirectory(external/mariadb-connector-c)

# SOCI - build as static for portable app bundle
set(SOCI_SHARED OFF CACHE BOOL "Build SOCI as static library" FORCE)
set(SOCI_TESTS OFF CACHE BOOL "Disable SOCI tests" FORCE)
set(SOCI_SQLITE3 ON CACHE BOOL "Build SOCI SQLite3 backend" FORCE)
# Tell SOCI we already have SQLite3
set(SQLite3_FOUND TRUE CACHE BOOL "" FORCE)
set(SQLite3_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/external/sqlite3 CACHE PATH "" FORCE)
set(SQLite3_LIBRARIES sqlite3_static CACHE STRING "" FORCE)
# Tell SOCI to use our static MariaDB client for MySQL backend
set(MYSQL_FOUND TRUE CACHE BOOL "" FORCE)
# Include both source and build dirs (mariadb_version.h is generated in build dir)
set(MYSQL_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/mariadb-connector-c/include;${CMAKE_BINARY_DIR}/external/mariadb-connector-c/include" CACHE PATH "" FORCE)
set(MYSQL_LIBRARIES mariadbclient CACHE STRING "" FORCE)
add_subdirectory(external/soci)

# Ensure SOCI MySQL backend waits for MariaDB to build (for generated headers)
if(TARGET soci_mysql)
    add_dependencies(soci_mysql mariadbclient)
endif()

# hiredis
set(DISABLE_TESTS ON CACHE BOOL "Disable hiredis tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libs for hiredis" FORCE)
add_subdirectory(external/hiredis)

# mongo-c-driver (must be before mongo-cxx-driver)
set(ENABLE_AUTOMATIC_INIT_AND_CLEANUP OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "Disable mongo-c tests" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libs" FORCE)
add_subdirectory(external/mongo-c-driver)

# mongo-cxx-driver
set(ENABLE_TESTS OFF CACHE BOOL "Disable mongocxx tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libs for mongo" FORCE)
add_subdirectory(external/mongo-cxx-driver)

# GoogleTest
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(external/googletest)

# Use xdg-desktop-portal on Linux to avoid GTK3/GTK4 conflict
if(UNIX AND NOT APPLE)
    set(NFD_PORTAL ON CACHE BOOL "Use xdg-desktop-portal instead of GTK" FORCE)
endif()
add_subdirectory(external/nativefiledialog-extended)

enable_testing()

# Find system packages
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

# Platform-specific windowing
if(UNIX AND NOT APPLE)
    # Linux uses GTK4 for windowing
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED gtk4)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    find_package(X11 REQUIRED)
else()
    # macOS and Windows use GLFW
    find_package(glfw3 3.3 REQUIRED)
endif()

# Common ImGui sources
set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/ImGuiPack/3rdparty/imgui_imguicolortextedit/TextEditor.cpp
)

# Platform-specific ImGui backend
if(UNIX AND NOT APPLE)
    # Linux uses GTK4, no GLFW backend needed
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
else()
    # macOS and Windows use GLFW
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_glfw.cpp)
endif()

# ImGui Node Editor sources
set(IMGUI_NODE_EDITOR_SOURCES
    external/imgui-node-editor/imgui_node_editor.cpp
    external/imgui-node-editor/imgui_node_editor_api.cpp
    external/imgui-node-editor/crude_json.cpp
    external/imgui-node-editor/imgui_canvas.cpp
)

# ImAnim sources
set(IMANIM_SOURCES
    external/ImAnim/im_anim.cpp
)

# Platform-specific backend configuration
if(APPLE)
    # Metal backend for macOS
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)

    if(NOT METAL_LIBRARY)
        message(FATAL_ERROR "Metal framework not found. Make sure you're building on macOS 10.11 or later.")
    endif()

    message(STATUS "Using Metal backend for macOS")
    message(STATUS "Found Metal framework: ${METAL_LIBRARY}")
    message(STATUS "Found QuartzCore framework: ${QUARTZCORE_LIBRARY}")
    message(STATUS "Found Foundation framework: ${FOUNDATION_LIBRARY}")
    message(STATUS "Found AppKit framework: ${APPKIT_LIBRARY}")

    # Add Metal backend source
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_metal.mm)
elseif(UNIX AND NOT APPLE)
    # Linux uses GTK4 + OpenGL via libepoxy
    message(STATUS "Using GTK4 + OpenGL backend for Linux")
else()
    # Windows uses OpenGL
    find_package(OpenGL REQUIRED)
    message(STATUS "Using OpenGL backend for ${CMAKE_SYSTEM_NAME}")
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
endif()

# Application source files
set(APP_SOURCES
    src/main.cpp
    src/app_state.cpp
    src/embedded_images.cpp
    src/embedded_fonts.cpp

    # Database
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/postgres/postgres_database_node.cpp
    src/database/postgres/postgres_schema_node.cpp
    src/database/mysql.cpp
    src/database/mysql/mysql_database_node.cpp
    src/database/redis.cpp
    src/database/mongodb.cpp
    src/database/mongodb/mongodb_database_node.cpp
    src/database/db_factory.cpp
    src/database/db_utils.cpp
    src/database/sql_builder.cpp
    src/database/connection_factory.cpp

    # UI
    src/ui/db_connection_dialog.cpp
    src/ui/table_dialog.cpp
    src/ui/confirm_dialog.cpp
    src/ui/input_dialog.cpp
    src/ui/query_history.cpp
    src/ui/table_renderer.cpp
    src/ui/auto_complete_input.cpp
    src/ui/database_node.cpp
    src/ui/db_sidebar_new.cpp
    # Tabs
    src/ui/tab_manager.cpp
    src/ui/tab/tab.cpp
    src/ui/tab/sql_editor_tab.cpp
    src/ui/tab/table_viewer_tab.cpp
    src/ui/tab/diagram_tab.cpp

    # Utils
    src/utils/file_dialog.cpp
    src/utils/toggle_button.cpp
    src/utils/spinner.cpp
    src/utils/crypto.cpp
    src/utils/logger.cpp

    # License
    src/license/license_manager.cpp
    src/ui/license_dialog.cpp
)

# Add application.cpp and platform-specific files
list(APPEND APP_SOURCES src/application.cpp)

# Add platform-specific files
if(APPLE)
    list(APPEND APP_SOURCES src/platform/macos_platform.mm)
elseif(UNIX AND NOT APPLE)
    list(APPEND APP_SOURCES src/platform/linux_platform.cpp)
else()
    list(APPEND APP_SOURCES src/platform/default_platform.cpp)
endif()

# Include resource embedding functions
include(cmake/EmbedResources.cmake)

# Embed fonts and images
embed_fonts()
embed_images()

list(APPEND APP_SOURCES ${EMBEDDED_FONTS_FILE} ${EMBEDDED_IMAGES_FILE})

# Main application
add_executable(${PROJECT_NAME}
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUI_NODE_EDITOR_SOURCES}
    ${IMANIM_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    external/imgui
    external/imgui/backends
    external/ImGuiPack/3rdparty/imgui_imguicolortextedit
    external/imgui-node-editor
    external/ImAnim
    external/json/include
    external/soci/include
    external/IconFontCppHeaders
    external/hiredis
    external/cpp-httplib
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include
    external/nativefiledialog-extended/src/include
    external/mongo-cxx-driver/src
)

# Platform-specific include directories
if(UNIX AND NOT APPLE)
    target_include_directories(${PROJECT_NAME} PRIVATE ${GTK4_INCLUDE_DIRS} ${EPOXY_INCLUDE_DIRS})
endif()

# Link libraries (common)
target_link_libraries(${PROJECT_NAME} PRIVATE
    sqlite3_static
    soci_sqlite3
    soci_postgresql
    soci_mysql
    nlohmann_json::nlohmann_json
    nfd
    OpenSSL::SSL
    OpenSSL::Crypto
    hiredis
    mongocxx_static
    bsoncxx_static
)

# Platform-specific linking
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        glfw
        ${METAL_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY}
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${GTK4_LIBRARIES}
        ${EPOXY_LIBRARIES}
        ${X11_LIBRARIES}
    )
    target_link_directories(${PROJECT_NAME} PRIVATE ${GTK4_LIBRARY_DIRS} ${EPOXY_LIBRARY_DIRS})
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        glfw
        OpenGL::GL
    )
endif()

# Set macOS specific properties
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        OUTPUT_NAME "${APP_NAME}"
    )

    # Copy the icon file to the Resources directory
    set_source_files_properties(assets/AppIcon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(${PROJECT_NAME} PRIVATE assets/AppIcon.icns)
endif()

add_executable(database_tests
    tests/database/sqlite_database_test.cpp
    tests/database/postgres_database_test.cpp
    tests/database/mysql_database_test.cpp
    tests/database/redis_database_test.cpp
    tests/database/mongodb_database_test.cpp
    tests/database/sql_builder_test.cpp
    tests/database/connection_factory_test.cpp
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/postgres/postgres_database_node.cpp
    src/database/postgres/postgres_schema_node.cpp
    src/database/mysql.cpp
    src/database/mysql/mysql_database_node.cpp
    src/database/redis.cpp
    src/database/mongodb.cpp
    src/database/mongodb/mongodb_database_node.cpp
    src/database/db_utils.cpp
    src/database/sql_builder.cpp
    src/database/connection_factory.cpp
    src/utils/logger.cpp
)

target_include_directories(database_tests PRIVATE
    include
    external/soci/include
    external/hiredis
    external/mongo-cxx-driver/src
    tests/database
)

target_link_libraries(database_tests PRIVATE
    gtest_main
    sqlite3_static
    soci_sqlite3
    soci_postgresql
    soci_mysql
    PostgreSQL::PostgreSQL
    hiredis
    mongocxx_static
    bsoncxx_static
)

add_test(NAME database_tests COMMAND database_tests)

# Install rules for Linux packaging
if(UNIX AND NOT APPLE)
    install(TARGETS ${PROJECT_NAME} DESTINATION bin)
    install(FILES packaging/flatpak/io.gitlab.dunkbing.dearsql.desktop
            DESTINATION share/applications)
    install(FILES packaging/flatpak/io.gitlab.dunkbing.dearsql.metainfo.xml
            DESTINATION share/metainfo)
    install(FILES assets/appicon.png
            DESTINATION share/icons/hicolor/256x256/apps
            RENAME io.gitlab.dunkbing.dearsql.png)
endif()
