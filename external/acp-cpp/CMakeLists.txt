cmake_minimum_required(VERSION 3.23)
project(acp-cpp VERSION 0.1.0 LANGUAGES CXX)

# registry download support pulls in cpp-httplib and OpenSSL; the core protocol
# needs only nlohmann_json and threads
option(ACP_REGISTRY "Build registry fetch/install (needs httplib + OpenSSL)" OFF)
option(ACP_BUILD_EXAMPLES "Build the example client" OFF)

if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()
find_package(Threads REQUIRED)

add_library(acp STATIC
    src/agents.cpp
    src/connection.cpp
    src/log.cpp
    src/process.cpp
    src/registry.cpp
    src/types.cpp
)
add_library(acp::acp ALIAS acp)
target_compile_features(acp PUBLIC cxx_std_20)
target_include_directories(acp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(acp PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

if(ACP_REGISTRY)
    if(NOT TARGET httplib::httplib)
        find_package(httplib CONFIG REQUIRED)
    endif()
    if(NOT TARGET OpenSSL::Crypto)
        find_package(OpenSSL REQUIRED)
    endif()
    target_compile_definitions(acp PRIVATE ACP_REGISTRY=1)
    target_link_libraries(acp PRIVATE httplib::httplib OpenSSL::SSL OpenSSL::Crypto)
endif()

if(ACP_BUILD_EXAMPLES)
    add_executable(acp-chat examples/chat.cpp)
    target_link_libraries(acp-chat PRIVATE acp::acp)
endif()
