cmake_minimum_required(VERSION 3.23)
project(dear-sql)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependencies
add_subdirectory(external/json)
add_subdirectory(external/soci)
set(DISABLE_TESTS ON CACHE BOOL "Disable hiredis tests" FORCE)
add_subdirectory(external/hiredis)
add_subdirectory(external/nativefiledialog-extended)
add_subdirectory(external/googletest)

enable_testing()

# Find system packages
find_package(glfw3 3.3 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

# Common ImGui sources
set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/ImGuiColorTextEdit/TextEditor.cpp
    external/ImGuiColorTextEdit/LanguageDefinitions.cpp
)

# ImGui Node Editor sources
set(IMGUI_NODE_EDITOR_SOURCES
    external/imgui-node-editor/imgui_node_editor.cpp
    external/imgui-node-editor/imgui_node_editor_api.cpp
    external/imgui-node-editor/crude_json.cpp
    external/imgui-node-editor/imgui_canvas.cpp
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

    # Define preprocessor macro
    add_definitions(-DUSE_METAL_BACKEND)
else()
    # OpenGL backend for other platforms
    find_package(OpenGL REQUIRED)

    message(STATUS "Using OpenGL backend for ${CMAKE_SYSTEM_NAME}")

    # Add OpenGL backend source
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)

    # Define preprocessor macro
    add_definitions(-DUSE_OPENGL_BACKEND)
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
    src/database/mysql.cpp
    src/database/redis.cpp
    src/database/db_factory.cpp
    src/database/db_utils.cpp

    # UI
    src/ui/db_sidebar.cpp
    src/ui/db_connection_dialog.cpp
    src/ui/table_dialog.cpp
    src/ui/drop_column_dialog.cpp
    src/ui/table_renderer.cpp
    src/ui/auto_complete_input.cpp
    src/ui/hierarchy_helpers.cpp
    src/ui/sqlite_hierarchy.cpp
    src/ui/postgres_hierarchy.cpp
    src/ui/mysql_hierarchy.cpp
    # New refactored hierarchy system
    src/ui/new/database_node.cpp
    src/ui/new/db_sidebar_new.cpp
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
)

# Add application.cpp and platform-specific files
list(APPEND APP_SOURCES src/application.cpp)

# Add platform-specific files
if(APPLE)
    list(APPEND APP_SOURCES src/platform/macos_platform.mm)
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
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    external/imgui
    external/imgui/backends
    external/ImGuiColorTextEdit
    external/imgui-node-editor
    external/json/include
    external/soci/include
    external/IconFontCppHeaders
    external/hiredis
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include
    external/nativefiledialog-extended/src/include
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    soci_sqlite3
    soci_postgresql
    soci_mysql
    nlohmann_json::nlohmann_json
    nfd
    OpenSSL::SSL
    OpenSSL::Crypto
    hiredis
)

# Platform-specific linking
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${METAL_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY}
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        OpenGL::GL
    )
endif()

# Set macOS specific properties
if(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "dearsql.db99.dev"
        MACOSX_BUNDLE_BUNDLE_NAME "DearSQL"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        OUTPUT_NAME "DearSQL"
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
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/mysql.cpp
    src/database/db_utils.cpp
    src/utils/logger.cpp
)

target_include_directories(database_tests PRIVATE
    include
    external/soci/include
    tests/database
)

target_link_libraries(database_tests PRIVATE
    gtest_main
    soci_sqlite3
    soci_postgresql
    soci_mysql
    PostgreSQL::PostgreSQL
)

add_test(NAME database_tests COMMAND database_tests)
