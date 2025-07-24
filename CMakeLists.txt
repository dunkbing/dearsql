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
find_package(OpenSSL REQUIRED)

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
    external/ImGuiColorTextEdit/TextEditor.cpp
    external/ImGuiColorTextEdit/LanguageDefinitions.cpp
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
    src/embedded_images.cpp

    # Database
    src/database/query_executor.cpp
    src/database/sqlite.cpp
    src/database/postgresql.cpp
    src/database/mysql.cpp
    src/database/db_factory.cpp

    # UI
    src/ui/db_sidebar.cpp
    src/ui/db_connection_dialog.cpp
    src/ui/tab.cpp
    src/ui/tab_manager.cpp
    src/ui/sqlite_hierarchy.cpp
    src/ui/postgres_hierarchy.cpp
    src/ui/mysql_hierarchy.cpp

    # Utils
    src/utils/file_dialog.cpp
    src/utils/toggle_button.cpp
    src/utils/spinner.cpp
    src/utils/crypto.cpp
)

# Add application.cpp and platform-specific files
list(APPEND APP_SOURCES src/application.cpp)

# Add platform-specific files
if(APPLE)
    list(APPEND APP_SOURCES src/platform/macos_platform.mm)
else()
    list(APPEND APP_SOURCES src/platform/default_platform.cpp)
endif()

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

# Embed image files as resources
file(GLOB IMAGE_FILES "assets/images/*.png" "assets/images/*.jpg" "assets/images/*.jpeg")
set(IMAGE_RESOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/embedded_images.cpp")

file(WRITE ${IMAGE_RESOURCE_FILE} "#include <cstdint>\n#include <cstddef>\n\n")
file(APPEND ${IMAGE_RESOURCE_FILE} "struct EmbeddedImage {\n")
file(APPEND ${IMAGE_RESOURCE_FILE} "    const char* name;\n")
file(APPEND ${IMAGE_RESOURCE_FILE} "    const uint8_t* data;\n")
file(APPEND ${IMAGE_RESOURCE_FILE} "    size_t size;\n")
file(APPEND ${IMAGE_RESOURCE_FILE} "};\n\n")

if(IMAGE_FILES)
    foreach(IMAGE_FILE ${IMAGE_FILES})
        get_filename_component(IMAGE_NAME ${IMAGE_FILE} NAME_WE)
        string(MAKE_C_IDENTIFIER ${IMAGE_NAME} IMAGE_VAR)

        file(READ ${IMAGE_FILE} IMAGE_DATA HEX)
        string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," IMAGE_DATA ${IMAGE_DATA})
        string(REGEX REPLACE ",$" "" IMAGE_DATA ${IMAGE_DATA})

        file(APPEND ${IMAGE_RESOURCE_FILE} "static const uint8_t ${IMAGE_VAR}_data[] = {${IMAGE_DATA}};\n")
    endforeach()

    file(APPEND ${IMAGE_RESOURCE_FILE} "\nstatic const EmbeddedImage embedded_images[] = {\n")
    foreach(IMAGE_FILE ${IMAGE_FILES})
        get_filename_component(IMAGE_NAME ${IMAGE_FILE} NAME_WE)
        string(MAKE_C_IDENTIFIER ${IMAGE_NAME} IMAGE_VAR)
        file(APPEND ${IMAGE_RESOURCE_FILE} "    {\"${IMAGE_NAME}\", ${IMAGE_VAR}_data, sizeof(${IMAGE_VAR}_data)},\n")
    endforeach()
    file(APPEND ${IMAGE_RESOURCE_FILE} "};\n\n")

    file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" const EmbeddedImage* getEmbeddedImages() { return embedded_images; }\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" size_t getEmbeddedImageCount() { return sizeof(embedded_images) / sizeof(embedded_images[0]); }\n")
else()
    file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" const EmbeddedImage* getEmbeddedImages() { return nullptr; }\n")
    file(APPEND ${IMAGE_RESOURCE_FILE} "extern \"C\" size_t getEmbeddedImageCount() { return 0; }\n")
endif()

list(APPEND APP_SOURCES ${RESOURCE_FILE} ${IMAGE_RESOURCE_FILE})

# Main application
add_executable(${PROJECT_NAME}
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    external/imgui
    external/imgui/backends
    external/ImGuiColorTextEdit
    external/json/include
    external/soci/include
    external/IconFontCppHeaders
    external/stb
    ${CMAKE_CURRENT_BINARY_DIR}
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
