# android build: the desktop sources minus what android can't run (subprocesses, the terminal
# modes, sentry) as a shared library that NativeActivity loads, packed into an apk by imshell.
# configure with the ndk toolchain chainloaded by vcpkg (docs/ANDROID.md)
set(ANDROID_SOURCES ${APP_SOURCES})
list(FILTER ANDROID_SOURCES EXCLUDE REGEX
     "src/main\\.cpp|src/platform/|src/cli/(cli|lsp|tui)\\.cpp|sentry_init|cli_command")
list(APPEND ANDROID_SOURCES src/ui/title_bar.cpp src/platform/android_main.cpp
                            src/platform/android_dialogs.cpp)
imshell_embed_fonts(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
imshell_embed_images(${CMAKE_CURRENT_SOURCE_DIR}/assets/images)
list(APPEND ANDROID_SOURCES ${EMBEDDED_FONTS_FILE} ${EMBEDDED_IMAGES_FILE})

add_library(${PROJECT_NAME} SHARED ${ANDROID_SOURCES} ${IMGUI_SOURCES} ${IMGUI_NODE_EDITOR_SOURCES})
target_compile_definitions(${PROJECT_NAME} PRIVATE DEARSQL_MOBILE)

add_subdirectory(external/acp-cpp)
add_library(imgui INTERFACE)
target_include_directories(imgui INTERFACE external/imgui)
set(IMGUI_RICHMD_STB_IMAGE_IMPLEMENTATION OFF CACHE BOOL "" FORCE)
add_subdirectory(external/imgui_rich_md EXCLUDE_FROM_ALL)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    external/imgui
    external/imgui/backends
    external/imgui-node-editor
    external/imgui-node-editor/external/stb_image
    external/IconFontCppHeaders
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include)

target_link_libraries(${PROJECT_NAME} PRIVATE
    unofficial::sqlite3::sqlite3
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    dearsql::dearsql
    imshell::imshell
    imshell::editor
    httplib::httplib
    imgui_rich_md
    acp::acp
    miniocpp::miniocpp
    libssh2::libssh2
    unofficial::tree-sitter::tree-sitter
    tree-sitter-sql-grammar
    tree-sitter-json-grammar
    spdlog::spdlog
    log)

imshell_android_apk(${PROJECT_NAME} PACKAGE dev.dearsql.app LABEL "${APP_NAME}"
                   VERSION ${APP_VERSION}
                   PERMISSIONS android.permission.INTERNET android.permission.ACCESS_NETWORK_STATE)
