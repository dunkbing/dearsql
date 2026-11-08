set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

set(SENTRY_BACKEND "breakpad")

# duckdb is most of the binary: build it for size and without parquet
# (read_parquet then autoloads the extension, needs network on first use)
if(PORT STREQUAL "duckdb")
    set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DSKIP_EXTENSIONS=parquet)
    # msvc: last /O wins, after vcpkg's /O2
    set(VCPKG_C_FLAGS_RELEASE /O1)
    set(VCPKG_CXX_FLAGS_RELEASE /O1)
endif()

# ci builds only release (half the build time and cache); local debug builds need both.
# passthrough hashes the value into the abi, so the two modes never share a package
list(APPEND VCPKG_ENV_PASSTHROUGH DEARSQL_VCPKG_RELEASE_ONLY)
if(DEFINED ENV{DEARSQL_VCPKG_RELEASE_ONLY})
    set(VCPKG_BUILD_TYPE release)
endif()
