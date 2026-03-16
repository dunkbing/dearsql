# FreeTDS db-lib for MSSQL support (static linking)
#
# Sets: SYBDB_LIBRARY, SYBDB_INCLUDE_DIR, SYBDB_DEPS

if(LINUX)
    find_library(SYBDB_LIBRARY NAMES libsybdb.a sybdb REQUIRED)
    find_path(SYBDB_INCLUDE_DIR sybdb.h REQUIRED)
    # transitive deps of static libsybdb.a (TLS, crypto, Kerberos)
    find_library(GNUTLS_LIBRARY gnutls REQUIRED)
    find_library(NETTLE_LIBRARY nettle REQUIRED)
    find_library(HOGWEED_LIBRARY hogweed REQUIRED)
    find_library(GMP_LIBRARY gmp REQUIRED)
    find_library(GSSAPI_LIBRARY gssapi_krb5 REQUIRED)
    set(SYBDB_DEPS ${GNUTLS_LIBRARY} ${NETTLE_LIBRARY} ${HOGWEED_LIBRARY} ${GMP_LIBRARY} ${GSSAPI_LIBRARY})
elseif(WIN32)
    # build FreeTDS db-lib from submodule (vcpkg freetds conflicts with libmariadb)
    include(ExternalProject)
    set(FREETDS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/freetds")
    set(FREETDS_INSTALL_DIR "${CMAKE_BINARY_DIR}/freetds-install")
    set(FREETDS_ICONV_DIR "${FREETDS_SOURCE_DIR}/iconv")
    # derive vcpkg OpenSSL root from OPENSSL_INCLUDE_DIR (set by find_package(OpenSSL))
    get_filename_component(_VCPKG_OPENSSL_ROOT "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    # find vcpkg libiconv and set up the iconv/ dir structure FreeTDS expects
    find_library(_ICONV_LIB NAMES iconv libiconv REQUIRED)
    find_path(_ICONV_INC iconv.h REQUIRED)
    file(MAKE_DIRECTORY "${FREETDS_ICONV_DIR}/lib")
    file(MAKE_DIRECTORY "${FREETDS_ICONV_DIR}/include")
    file(COPY_FILE "${_ICONV_LIB}" "${FREETDS_ICONV_DIR}/lib/iconv.lib" ONLY_IF_DIFFERENT)
    file(COPY "${_ICONV_INC}/iconv.h" DESTINATION "${FREETDS_ICONV_DIR}/include")
    ExternalProject_Add(freetds_external
        SOURCE_DIR "${FREETDS_SOURCE_DIR}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM "${CMAKE_GENERATOR_PLATFORM}"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${FREETDS_INSTALL_DIR}
            -DOPENSSL_ROOT_DIR=${_VCPKG_OPENSSL_ROOT}
            -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
            -DCMAKE_C_FLAGS_RELEASE=/MT
            -DCMAKE_C_FLAGS_DEBUG=/MTd
            -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release --target db-lib tds replacements tdsutils
        INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/lib
            # merge all FreeTDS static libs into one (db-lib depends on tds, replacements, tdsutils)
            COMMAND lib /NOLOGO /OUT:${FREETDS_INSTALL_DIR}/lib/sybdb.lib
                <BINARY_DIR>/src/dblib/Release/db-lib.lib
                <BINARY_DIR>/src/tds/Release/tds.lib
                <BINARY_DIR>/src/replacements/Release/replacements.lib
                <BINARY_DIR>/src/utils/Release/tdsutils.lib
            COMMAND ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/include
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${FREETDS_SOURCE_DIR}/include ${FREETDS_INSTALL_DIR}/include
            COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/config.h ${FREETDS_INSTALL_DIR}/include/config.h
            COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/tds_sysdep_public.h ${FREETDS_INSTALL_DIR}/include/tds_sysdep_public.h
        BUILD_BYPRODUCTS "${FREETDS_INSTALL_DIR}/lib/sybdb.lib"
    )
    add_library(freetds_sybdb STATIC IMPORTED)
    set_target_properties(freetds_sybdb PROPERTIES
        IMPORTED_LOCATION "${FREETDS_INSTALL_DIR}/lib/sybdb.lib"
    )
    add_dependencies(freetds_sybdb freetds_external)
    set(SYBDB_LIBRARY freetds_sybdb)
    set(SYBDB_INCLUDE_DIR "${FREETDS_INSTALL_DIR}/include")
    set(SYBDB_DEPS ws2_32 crypt32 ${_ICONV_LIB} legacy_stdio_definitions.lib)
elseif(APPLE)
    # build FreeTDS db-lib from submodule with correct deployment target
    include(ExternalProject)
    set(FREETDS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/freetds")
    set(FREETDS_INSTALL_DIR "${CMAKE_BINARY_DIR}/freetds-install")
    # point FreeTDS at vcpkg's OpenSSL (not Homebrew's)
    get_target_property(_OPENSSL_SSL_LOC OpenSSL::SSL IMPORTED_LOCATION)
    get_filename_component(_VCPKG_OPENSSL_LIB_DIR "${_OPENSSL_SSL_LOC}" DIRECTORY)
    get_filename_component(_VCPKG_OPENSSL_ROOT "${_VCPKG_OPENSSL_LIB_DIR}" DIRECTORY)
    ExternalProject_Add(freetds_external
        SOURCE_DIR "${FREETDS_SOURCE_DIR}"
        CMAKE_ARGS
            -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -DCMAKE_OSX_ARCHITECTURES=arm64
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=${FREETDS_INSTALL_DIR}
            -DCMAKE_C_FLAGS=-mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -DOPENSSL_ROOT_DIR=${_VCPKG_OPENSSL_ROOT}
            -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew
            -DCMAKE_SYSTEM_IGNORE_PREFIX_PATH=/opt/homebrew
        # only build the static db-lib target (skip shared libs that fail to link)
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target db-lib
        INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/lib
            # merge all FreeTDS static libs into one (db-lib depends on tds, replacements, tdsutils)
            COMMAND libtool -static -o ${FREETDS_INSTALL_DIR}/lib/libsybdb.a
                <BINARY_DIR>/src/dblib/libdb-lib.a
                <BINARY_DIR>/src/tds/libtds.a
                <BINARY_DIR>/src/replacements/libreplacements.a
                <BINARY_DIR>/src/utils/libtdsutils.a
            COMMAND ${CMAKE_COMMAND} -E make_directory ${FREETDS_INSTALL_DIR}/include
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${FREETDS_SOURCE_DIR}/include ${FREETDS_INSTALL_DIR}/include
            COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/config.h ${FREETDS_INSTALL_DIR}/include/config.h
            COMMAND ${CMAKE_COMMAND} -E copy <BINARY_DIR>/include/tds_sysdep_public.h ${FREETDS_INSTALL_DIR}/include/tds_sysdep_public.h
        BUILD_BYPRODUCTS "${FREETDS_INSTALL_DIR}/lib/libsybdb.a"
    )
    add_library(freetds_sybdb STATIC IMPORTED)
    set_target_properties(freetds_sybdb PROPERTIES
        IMPORTED_LOCATION "${FREETDS_INSTALL_DIR}/lib/libsybdb.a"
    )
    add_dependencies(freetds_sybdb freetds_external)
    set(SYBDB_LIBRARY freetds_sybdb)
    set(SYBDB_INCLUDE_DIR "${FREETDS_INSTALL_DIR}/include")
    find_library(KERBEROS_FRAMEWORK Kerberos)
    find_library(ICONV_LIBRARY iconv)
    set(SYBDB_DEPS ${KERBEROS_FRAMEWORK} ${ICONV_LIBRARY})
endif()
