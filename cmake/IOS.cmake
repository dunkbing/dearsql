# ipad build: the desktop sources minus what iOS can't run (subprocesses, the
# terminal modes, sparkle, sentry) and the backends without a driver here.
# configure with -DCMAKE_SYSTEM_NAME=iOS -DVCPKG_TARGET_TRIPLET=arm64-ios-simulator
set(IOS_SOURCES ${APP_SOURCES})
list(FILTER IOS_SOURCES EXCLUDE REGEX
     "src/main\\.cpp|src/platform/|src/cli/(cli|lsp|tui)\\.cpp|sentry_init|cli_command")
list(APPEND IOS_SOURCES src/platform/ios_platform.mm src/platform/ios_dialogs.mm)
imshell_embed_fonts(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts)
imshell_embed_images(${CMAKE_CURRENT_SOURCE_DIR}/assets/images)
list(APPEND IOS_SOURCES ${EMBEDDED_FONTS_FILE} ${EMBEDDED_IMAGES_FILE})

add_executable(${PROJECT_NAME} MACOSX_BUNDLE ${IOS_SOURCES} ${IMGUI_SOURCES})
target_compile_definitions(${PROJECT_NAME} PRIVATE DEARSQL_MOBILE DEARSQL_IOS)
include(cmake/ICloud.cmake)
dearsql_icloud(${PROJECT_NAME})
set_source_files_properties(src/platform/ios_platform.mm src/platform/ios_dialogs.mm
                            PROPERTIES COMPILE_OPTIONS -fobjc-arc)

add_subdirectory(external/acp-cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
    include
    ${IMSHELL_IMGUI_DIR}
    ${IMSHELL_IMGUI_DIR}/backends
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
    imshell::markdown
    imshell::node_editor
    acp::acp
    miniocpp::miniocpp
    libssh2::libssh2
    unofficial::tree-sitter::tree-sitter
    tree-sitter-sql-grammar
    tree-sitter-json-grammar
    spdlog::spdlog
    "-framework UIKit"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework Foundation"
    "-framework CoreGraphics"
    "-framework CFNetwork"
    "-framework Security"
    "-framework CoreData"
    "-framework GameController"
    "-framework UniformTypeIdentifiers")

configure_file(cmake/Info-iOS.plist.in ${CMAKE_BINARY_DIR}/Info-iOS.plist @ONLY)
set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info-iOS.plist
    OUTPUT_NAME "${APP_NAME}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")

# app icon: compile the asset catalog into the bundle (Assets.car + the small pngs)
if(CMAKE_OSX_SYSROOT MATCHES "simulator")
    set(ACTOOL_PLATFORM iphonesimulator)
else()
    set(ACTOOL_PLATFORM iphoneos)
endif()
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND xcrun actool ${CMAKE_SOURCE_DIR}/assets/ios/Assets.xcassets
            --compile $<TARGET_BUNDLE_DIR:${PROJECT_NAME}> --platform ${ACTOOL_PLATFORM}
            --minimum-deployment-target 17.0 --app-icon AppIcon
            --target-device iphone --target-device ipad
            --output-partial-info-plist ${CMAKE_BINARY_DIR}/actool-partial.plist
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/assets/ios/PrivacyInfo.xcprivacy
            $<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/PrivacyInfo.xcprivacy
    VERBATIM)
