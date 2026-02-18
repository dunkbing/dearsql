cmake_minimum_required(VERSION 3.23)

# vcpkg toolchain - must be set before project() call
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
    elseif(EXISTS "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
    endif()
endif()

# Auto-detect vcpkg triplet based on platform and architecture
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(APPLE)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(ARCH STREQUAL "arm64")
            set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "vcpkg triplet" FORCE)
        else()
            set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "vcpkg triplet" FORCE)
        endif()
    elseif(UNIX)
        execute_process(COMMAND uname -m OUTPUT_VARIABLE ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(ARCH STREQUAL "x86_64")
            set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "vcpkg triplet" FORCE)
        else()
            set(VCPKG_TARGET_TRIPLET "arm64-linux" CACHE STRING "vcpkg triplet" FORCE)
        endif()
    elseif(WIN32)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg triplet" FORCE)
    endif()
endif()

# macOS deployment target
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Use project-local triplet overlay for deployment target
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_SOURCE_DIR}/cmake/triplets" CACHE STRING "" FORCE)

project(dear-sql)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    # Enable experimental C++20 features (jthread, stop_token) in libc++
    add_compile_options(-fexperimental-library)
    add_link_options(-fexperimental-library)
endif()

# APP_METADATA
file(STRINGS "${CMAKE_SOURCE_DIR}/APP_METADATA" _metadata_lines)
foreach(_line IN LISTS _metadata_lines)
    if(_line MATCHES "^([A-Z_]+)=(.+)$")
        set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()

configure_file(
    ${CMAKE_SOURCE_DIR}/include/config.hpp.in
    ${CMAKE_BINARY_DIR}/include/config.hpp
    @ONLY
)

# =============================================================================
# vcpkg packages (cross-platform)
# =============================================================================
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(unofficial-libmysql CONFIG REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)

enable_testing()

# Platform-specific windowing
if(UNIX AND NOT APPLE)
    # Linux uses GTK4 for windowing
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK4 REQUIRED gtk4)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    find_package(X11 REQUIRED)
else()
    # macOS and Windows use GLFW
    find_package(glfw3 CONFIG REQUIRED)
endif()

# Sparkle auto-update (macOS only)
if(APPLE)
    set(SPARKLE_FRAMEWORK_PATH "${CMAKE_SOURCE_DIR}/external/Sparkle/Sparkle.framework")
    if(NOT EXISTS "${SPARKLE_FRAMEWORK_PATH}")
        message(FATAL_ERROR "Sparkle.framework not found at ${SPARKLE_FRAMEWORK_PATH}. "
            "Download from https://sparkle-project.org and extract into external/Sparkle/")
    endif()
    if(NOT DEFINED SPARKLE_ED_PUBLIC_KEY)
        set(SPARKLE_ED_PUBLIC_KEY "" CACHE STRING "Sparkle EdDSA public key for update verification")
    endif()
endif()

# =============================================================================
# ImGui (from submodule - tightly integrated with platform backends)
# =============================================================================
set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/ImGuiPack/3rdparty/imgui_imguicolortextedit/TextEditor.cpp
)

# Platform-specific ImGui backend
if(UNIX AND NOT APPLE)
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
else()
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_glfw.cpp)
endif()

# ImGui Node Editor sources (from submodule - custom fork)
set(IMGUI_NODE_EDITOR_SOURCES
    external/imgui-node-editor/imgui_node_editor.cpp
    external/imgui-node-editor/imgui_node_editor_api.cpp
    external/imgui-node-editor/crude_json.cpp
    external/imgui-node-editor/imgui_canvas.cpp
)

# Platform-specific backend configuration
if(APPLE)
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)

    message(STATUS "Using Metal backend for macOS")
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_metal.mm)
elseif(UNIX AND NOT APPLE)
    message(STATUS "Using GTK4 + OpenGL backend for Linux")
else()
    find_package(OpenGL REQUIRED)
    message(STATUS "Using OpenGL backend for ${CMAKE_SYSTEM_NAME}")
    list(APPEND IMGUI_SOURCES external/imgui/backends/imgui_impl_opengl3.cpp)
endif()

# =============================================================================
# Application source files
# =============================================================================
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
    src/database/ddl_builder.cpp

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
    list(APPEND APP_SOURCES src/platform/macos_connection_dialog.mm)
    list(APPEND APP_SOURCES src/platform/macos_updater.mm)
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

# =============================================================================
# Main application target
# =============================================================================
add_executable(${PROJECT_NAME}
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
    ${IMGUI_NODE_EDITOR_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    external/imgui
    external/imgui/backends
    external/ImGuiPack/3rdparty/imgui_imguicolortextedit
    external/imgui-node-editor
    external/IconFontCppHeaders
    external/cpp-httplib
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

# Platform-specific include directories
if(UNIX AND NOT APPLE)
    target_include_directories(${PROJECT_NAME} PRIVATE ${GTK4_INCLUDE_DIRS} ${EPOXY_INCLUDE_DIRS})
endif()

# Sparkle includes (macOS)
if(APPLE)
    target_include_directories(${PROJECT_NAME} PRIVATE "${SPARKLE_FRAMEWORK_PATH}/Headers")
endif()

# Link libraries (common - vcpkg packages)
target_link_libraries(${PROJECT_NAME} PRIVATE
    unofficial::sqlite3::sqlite3
    nlohmann_json::nlohmann_json
    PostgreSQL::PostgreSQL
    unofficial::libmysql::libmysql
    hiredis::hiredis
    $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
    $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>
    nfd::nfd
    OpenSSL::SSL
    OpenSSL::Crypto
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
    target_link_libraries(${PROJECT_NAME} PRIVATE "${SPARKLE_FRAMEWORK_PATH}")
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
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_IDENTIFIER}"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${APP_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${APP_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "AppIcon.icns"
        OUTPUT_NAME "${APP_NAME}"
        BUILD_RPATH "@loader_path/../Frameworks"
        INSTALL_RPATH "@loader_path/../Frameworks"
    )

    # Copy the icon file to the Resources directory
    set_source_files_properties(assets/AppIcon.icns PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(${PROJECT_NAME} PRIVATE assets/AppIcon.icns)

    # Embed Sparkle.framework into the app bundle
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${SPARKLE_FRAMEWORK_PATH}"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Frameworks/Sparkle.framework"
        COMMENT "Embedding Sparkle.framework into app bundle"
    )
endif()

# =============================================================================
# Test target
# =============================================================================
add_executable(database_tests
    tests/database/sqlite_database_test.cpp
    tests/database/postgres_database_test.cpp
    tests/database/mysql_database_test.cpp
    tests/database/redis_database_test.cpp
    tests/database/mongodb_database_test.cpp
    tests/database/sql_builder_test.cpp
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
    src/database/ddl_builder.cpp
    src/utils/logger.cpp
)

target_include_directories(database_tests PRIVATE
    include
    tests/database
)

target_link_libraries(database_tests PRIVATE
    GTest::gtest_main
    unofficial::sqlite3::sqlite3
    PostgreSQL::PostgreSQL
    unofficial::libmysql::libmysql
    hiredis::hiredis
    $<IF:$<TARGET_EXISTS:mongo::mongocxx_static>,mongo::mongocxx_static,mongo::mongocxx_shared>
    $<IF:$<TARGET_EXISTS:mongo::bsoncxx_static>,mongo::bsoncxx_static,mongo::bsoncxx_shared>
)

add_test(NAME database_tests COMMAND database_tests)

# =============================================================================
# Install rules for Linux packaging
# =============================================================================
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
