# DataStax C++ driver for Apache Cassandra
#
# Builds the driver from external/cassandra-cpp-driver as a static library. Depends on
# libuv (via vcpkg), OpenSSL, and zlib. The library target produced is
# `cassandra_static`.

set(CASS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(CASS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_INTEGRATION_TESTS OFF CACHE BOOL "" FORCE)
set(CASS_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(CASS_USE_OPENSSL ON CACHE BOOL "" FORCE)
# vcpkg's zlib installs as zlib.lib on every triplet, but the driver's
# Dependencies.cmake unconditionally rewrites zlib.lib -> zlibstatic.lib when
# WIN32 AND CASS_USE_STATIC_LIBS, breaking the link. Wire-protocol compression
# is optional, so disable zlib on Windows rather than fight the rename.
if(WIN32)
    set(CASS_USE_ZLIB OFF CACHE BOOL "" FORCE)
else()
    set(CASS_USE_ZLIB ON CACHE BOOL "" FORCE)
endif()
set(CASS_USE_KERBEROS OFF CACHE BOOL "" FORCE)
set(CASS_USE_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CASS_INSTALL_HEADER OFF CACHE BOOL "" FORCE)
set(CASS_INSTALL_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(CASS_MULTICORE_COMPILATION ON CACHE BOOL "" FORCE)

# vcpkg ships libuv; surface its include + library paths so the driver's
# FindLibuv.cmake (path-based) succeeds.
find_package(libuv CONFIG QUIET)
if(TARGET libuv::uv_a)
    get_target_property(_LIBUV_INCLUDE libuv::uv_a INTERFACE_INCLUDE_DIRECTORIES)
    set(LIBUV_ROOT_DIR "${_LIBUV_INCLUDE}/.." CACHE PATH "" FORCE)
endif()

# cassandra-cpp-driver's CMakeLists asserts on CMAKE_CXX_COMPILER_ID being one of
# Clang/GNU/MSVC; on macOS the actual value is "AppleClang", which is
# functionally Clang. Override locally for the subdirectory scope.
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(CMAKE_CXX_COMPILER_ID "Clang")
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
add_subdirectory(external/cassandra-cpp-driver EXCLUDE_FROM_ALL)

if(MSVC)
    target_compile_options(
        cassandra_static
        PRIVATE /FI${CMAKE_SOURCE_DIR}/cmake/msvc_cassandra_sparsehash_compat.hpp
    )
endif()

# GCC 13+ emits a false-positive -Wstringop-overread on the C++20 operator<=>
# path for vector<unsigned char> in token_map_impl.hpp. The driver builds with
# -Werror, so suppress just this diagnostic.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(
        cassandra_static
        PRIVATE -Wno-error=stringop-overread -Wno-stringop-overread
    )
endif()

# Clang 19+ emits -Wnontrivial-memcall for md5.cpp's memset(this, 0, sizeof(Md5)),
# and the driver builds with -Werror. -Wno-unknown-warning-option has to come
# first: older Clang does not know the flag below, and -Werror would turn that
# into an error too. Version-gating is unreliable, as Apple Clang version numbers
# do not track upstream LLVM.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(
        cassandra_static
        PRIVATE
            -Wno-unknown-warning-option
            -Wno-error=nontrivial-memcall
            -Wno-nontrivial-memcall
    )
endif()

target_compile_definitions(cassandra_static INTERFACE CASS_STATIC)

# The driver's CMakeLists builds CASS_INCLUDES privately; re-expose the public
# header via INTERFACE.
target_include_directories(
    cassandra_static
    INTERFACE ${CMAKE_SOURCE_DIR}/external/cassandra-cpp-driver/include
)
