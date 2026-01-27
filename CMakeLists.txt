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

# Use xdg-desktop-portal on Linux to avoid GTK3/GTK4 conflict
if(UNIX AND NOT APPLE)
    set(NFD_PORTAL ON CACHE BOOL "Use xdg-desktop-portal instead of GTK" FORCE)
endif()
add_subdirectory(external/nativefiledialog-extended)

enable_testing()

# Find system packages
find_package(SQLite3 REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)

# Platform-specific windowing
if(UNIX AND NOT APPLE)
    # Linux uses GTK4 for windowing
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED gtk4)
    pkg_check_modules(EPOXY REQUIRED epoxy)
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
    external/ImGuiColorTextEdit/TextEditor.cpp
    external/ImGuiColorTextEdit/LanguageDefinitions.cpp
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
    src/database/db_factory.cpp
    src/database/db_utils.cpp

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
    external/ImGuiColorTextEdit
    external/imgui-node-editor
    external/ImAnim
    external/json/include
    external/soci/include
    external/IconFontCppHeaders
    external/hiredis
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include
    external/nativefiledialog-extended/src/include
)

# Platform-specific include directories
if(UNIX AND NOT APPLE)
    target_include_directories(${PROJECT_NAME} PRIVATE ${GTK4_INCLUDE_DIRS} ${EPOXY_INCLUDE_DIRS})
endif()

# Link libraries (common)
target_link_libraries(${PROJECT_NAME} PRIVATE
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
