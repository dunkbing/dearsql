cmake_minimum_required(VERSION 3.23)
project(dear-sql)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies
add_subdirectory(external/json)
add_subdirectory(external/soci)

# Find system packages
find_package(glfw3 3.3 REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PostgreSQL REQUIRED)

# Native File Dialog Extended (as git submodule)
add_subdirectory(external/nativefiledialog-extended)

# GLFW + Backend (Metal on macOS, OpenGL elsewhere)
find_package(glfw3 3.3 REQUIRED)

# Common ImGui sources
set(IMGUI_SOURCES
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
)

# Platform-specific backend configuration
if(APPLE)
    # Metal backend for macOS
    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)

    if(NOT METAL_LIBRARY)
        message(FATAL_ERROR "Metal framework not found. Make sure you're building on macOS 10.11 or later.")
    endif()

    message(STATUS "Using Metal backend for macOS")
    message(STATUS "Found Metal framework: ${METAL_LIBRARY}")
    message(STATUS "Found QuartzCore framework: ${QUARTZCORE_LIBRARY}")
    message(STATUS "Found Foundation framework: ${FOUNDATION_LIBRARY}")

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

    # Database
    src/database/query_executor.cpp
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/db_factory.cpp

    # Tabs
    src/tabs/tab.cpp
    src/tabs/tab_manager.cpp

    # UI
    src/ui/db_sidebar.cpp
    src/ui/db_connection_dialog.cpp

    # Utils
    src/utils/file_dialog.cpp
    src/utils/toggle_button.cpp
)

# Use .mm extension for all platforms (Objective-C++ can compile C++ code)
list(APPEND APP_SOURCES src/application.mm)

# Embed font files as resources
file(GLOB FONT_FILES "assets/fonts/*.ttf" "assets/fonts/*.otf")
set(RESOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/embedded_fonts.cpp")

file(WRITE ${RESOURCE_FILE} "#include <cstdint>\n#include <cstddef>\n\n")
file(APPEND ${RESOURCE_FILE} "struct EmbeddedFont {\n")
file(APPEND ${RESOURCE_FILE} "    const char* name;\n")
file(APPEND ${RESOURCE_FILE} "    const uint8_t* data;\n")
file(APPEND ${RESOURCE_FILE} "    size_t size;\n")
file(APPEND ${RESOURCE_FILE} "};\n\n")

if(FONT_FILES)
    foreach(FONT_FILE ${FONT_FILES})
        get_filename_component(FONT_NAME ${FONT_FILE} NAME_WE)
        string(MAKE_C_IDENTIFIER ${FONT_NAME} FONT_VAR)

        file(READ ${FONT_FILE} FONT_DATA HEX)
        string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," FONT_DATA ${FONT_DATA})
        string(REGEX REPLACE ",$" "" FONT_DATA ${FONT_DATA})

        file(APPEND ${RESOURCE_FILE} "static const uint8_t ${FONT_VAR}_data[] = {${FONT_DATA}};\n")
    endforeach()

    file(APPEND ${RESOURCE_FILE} "\nstatic const EmbeddedFont embedded_fonts[] = {\n")
    foreach(FONT_FILE ${FONT_FILES})
        get_filename_component(FONT_NAME ${FONT_FILE} NAME_WE)
        string(MAKE_C_IDENTIFIER ${FONT_NAME} FONT_VAR)
        file(APPEND ${RESOURCE_FILE} "    {\"${FONT_NAME}\", ${FONT_VAR}_data, sizeof(${FONT_VAR}_data)},\n")
    endforeach()
    file(APPEND ${RESOURCE_FILE} "};\n\n")

    file(APPEND ${RESOURCE_FILE} "extern \"C\" const EmbeddedFont* getEmbeddedFonts() { return embedded_fonts; }\n")
    file(APPEND ${RESOURCE_FILE} "extern \"C\" size_t getEmbeddedFontCount() { return sizeof(embedded_fonts) / sizeof(embedded_fonts[0]); }\n")
else()
    file(APPEND ${RESOURCE_FILE} "extern \"C\" const EmbeddedFont* getEmbeddedFonts() { return nullptr; }\n")
    file(APPEND ${RESOURCE_FILE} "extern \"C\" size_t getEmbeddedFontCount() { return 0; }\n")
endif()

list(APPEND APP_SOURCES ${RESOURCE_FILE})

# Main application
add_executable(${PROJECT_NAME}
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    external/imgui
    external/imgui/backends
    external/json/include
    external/soci/include
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Link libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    glfw
    soci_sqlite3
    soci_postgresql
    nlohmann_json::nlohmann_json
    nfd
)

# Platform-specific linking
if(APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${METAL_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ${FOUNDATION_LIBRARY}
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
        MACOSX_BUNDLE_BUNDLE_NAME "Dear SQL"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
endif()
