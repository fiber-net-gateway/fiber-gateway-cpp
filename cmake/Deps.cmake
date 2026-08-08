include_guard()

include(ExternalProject)
include(FetchContent)

set(FIBER_BORINGSSL_URL
    "https://codeload.github.com/google/boringssl/tar.gz/refs/tags/0.20251124.0"
    CACHE STRING "BoringSSL source archive URL")
set(FIBER_BORINGSSL_SHA256
    "d47f89b894bf534c82071d7426c5abf1e5bd044fee242def53cd5d3d0f656c09"
    CACHE STRING "BoringSSL source archive SHA-256")
set(FIBER_ZLIB_URL
    "https://codeload.github.com/madler/zlib/tar.gz/refs/tags/v1.3.1"
    CACHE STRING "zlib source archive URL")
set(FIBER_ZLIB_SHA256
    "17e88863f3600672ab49182f217281b6fc4d3c762bde361935e436a95214d05c"
    CACHE STRING "zlib source archive SHA-256")
set(FIBER_PROTOBUF_URL
    "https://codeload.github.com/protocolbuffers/protobuf/tar.gz/refs/tags/v21.12"
    CACHE STRING "protobuf source archive URL")
set(FIBER_PROTOBUF_SHA256
    "22fdaf641b31655d4b2297f9981fa5203b2866f8332d3c6333f6b0107bb320de"
    CACHE STRING "protobuf source archive SHA-256")
set(FIBER_GOOGLETEST_URL
    "https://codeload.github.com/google/googletest/zip/refs/tags/v1.14.0"
    CACHE STRING "GoogleTest source archive URL")
set(FIBER_GOOGLETEST_SHA256
    "1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4"
    CACHE STRING "GoogleTest source archive SHA-256")
set(FIBER_JEMALLOC_URL
    "https://github.com/jemalloc/jemalloc/releases/download/5.3.0/jemalloc-5.3.0.tar.bz2"
    CACHE STRING "jemalloc source archive URL")
set(FIBER_JEMALLOC_SHA256
    "2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa"
    CACHE STRING "jemalloc source archive SHA-256")

if (NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_CURRENT_LIST_DIR}/../temp/_deps" CACHE PATH "FetchContent base directory")
endif()

function(fiber_clear_invalid_source_dir name)
    string(TOUPPER "${name}" name_upper)
    set(var "FETCHCONTENT_SOURCE_DIR_${name_upper}")
    if (DEFINED ${var})
        if ("${${var}}" STREQUAL "" OR NOT EXISTS "${${var}}/CMakeLists.txt")
            unset(${var} CACHE)
        endif()
    endif()
endfunction()

function(fiber_use_cached_content name)
    string(TOUPPER "${name}" name_upper)
    set(var "FETCHCONTENT_SOURCE_DIR_${name_upper}")
    fiber_clear_invalid_source_dir("${name}")
    if (NOT DEFINED ${var})
        set(src "${FETCHCONTENT_BASE_DIR}/${name}-src")
        if (EXISTS "${src}/CMakeLists.txt")
            set(${var} "${src}" CACHE PATH "Use cached ${name} source directory")
        endif()
    endif()
endfunction()

function(fiber_purge_cache_regex pattern)
    get_cmake_property(cache_vars CACHE_VARIABLES)
    foreach(cache_var IN LISTS cache_vars)
        if (cache_var MATCHES "${pattern}")
            unset(${cache_var} CACHE)
        endif()
    endforeach()
endfunction()

function(fiber_prepare_jemalloc_target)
    if (TARGET fiber_jemalloc)
        return()
    endif()

    set(FIBER_JEMALLOC_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/jemalloc-src")
    set(FIBER_JEMALLOC_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/jemalloc-build")
    set(FIBER_JEMALLOC_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/jemalloc-install")

    ExternalProject_Add(
        fiber_jemalloc_ep
        URL "${FIBER_JEMALLOC_URL}"
        URL_HASH "SHA256=${FIBER_JEMALLOC_SHA256}"
        SOURCE_DIR "${FIBER_JEMALLOC_SOURCE_DIR}"
        BINARY_DIR "${FIBER_JEMALLOC_BINARY_DIR}"
        INSTALL_DIR "${FIBER_JEMALLOC_INSTALL_DIR}"
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER}
            <SOURCE_DIR>/configure
            --prefix=<INSTALL_DIR>
            --disable-shared
            --enable-static
        BUILD_COMMAND ${CMAKE_MAKE_PROGRAM}
        INSTALL_COMMAND ${CMAKE_MAKE_PROGRAM} install
        BUILD_BYPRODUCTS "${FIBER_JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a"
        UPDATE_DISCONNECTED ON
    )

    add_library(fiber_jemalloc UNKNOWN IMPORTED GLOBAL)
    set_target_properties(fiber_jemalloc PROPERTIES
        IMPORTED_LOCATION "${FIBER_JEMALLOC_INSTALL_DIR}/lib/libjemalloc.a")
endfunction()

set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set_property(GLOBAL PROPERTY ALLOW_DUPLICATE_CUSTOM_TARGETS ON)

set(BORINGSSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BORINGSSL_INSTALL OFF CACHE BOOL "" FORCE)
fiber_use_cached_content(boringssl)
FetchContent_Declare(
    boringssl
    URL "${FIBER_BORINGSSL_URL}"
    URL_HASH "SHA256=${FIBER_BORINGSSL_SHA256}"
)
FetchContent_MakeAvailable(boringssl)

if (TARGET ssl AND NOT TARGET boringssl::ssl)
    add_library(boringssl::ssl ALIAS ssl)
endif()
if (TARGET crypto AND NOT TARGET boringssl::crypto)
    add_library(boringssl::crypto ALIAS crypto)
endif()

set(BORINGSSL_INCLUDE_DIR "${boringssl_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(BORINGSSL_LIBRARY_DIR "${boringssl_BINARY_DIR}" CACHE PATH "" FORCE)
set(BORINGSSL_ROOT_DIR "${boringssl_BINARY_DIR}" CACHE PATH "" FORCE)

# Populate zlib for external consumers such as scripts/build_nginx.sh.  Keep it
# out of this project's target graph until fiber_lib has an in-tree gzip user.
fiber_use_cached_content(zlib)
FetchContent_Declare(
    zlib
    URL "${FIBER_ZLIB_URL}"
    URL_HASH "SHA256=${FIBER_ZLIB_SHA256}"
    SOURCE_SUBDIR fiber_download_only
)
FetchContent_MakeAvailable(zlib)
set(FIBER_ZLIB_SOURCE_DIR "${zlib_SOURCE_DIR}" CACHE PATH "Downloaded zlib source directory" FORCE)

# ---- optional protobuf-lite runtime + protoc codegen ----
# Nacos is the only protobuf consumer. Keep the dependency out of the target
# graph until a proto library is actually requested.
function(fiber_prepare_protobuf_target)
    if(TARGET protobuf::libprotobuf-lite AND (TARGET protobuf::protoc OR TARGET protoc))
        if(TARGET protobuf::protoc)
            set(FIBER_PROTOC_TARGET "protobuf::protoc" CACHE STRING "protoc target used by fiber_proto_library()")
        else()
            set(FIBER_PROTOC_TARGET "protoc" CACHE STRING "protoc target used by fiber_proto_library()")
        endif()
        return()
    endif()

    # v21.12 is the last release before protobuf made abseil-cpp a hard
    # dependency. Build it as C++17 even though Fiber itself is C++23; the
    # function scope keeps these settings from leaking back to Fiber targets.
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
    set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
    fiber_use_cached_content(protobuf)
    FetchContent_Declare(
        protobuf
        URL "${FIBER_PROTOBUF_URL}"
        URL_HASH "SHA256=${FIBER_PROTOBUF_SHA256}"
    )
    FetchContent_MakeAvailable(protobuf)

    if(NOT TARGET protobuf::libprotobuf-lite AND TARGET libprotobuf-lite)
        add_library(protobuf::libprotobuf-lite ALIAS libprotobuf-lite)
    endif()
    if(TARGET protobuf::protoc)
        set(FIBER_PROTOC_TARGET "protobuf::protoc" CACHE STRING "protoc target used by fiber_proto_library()")
    elseif(TARGET protoc)
        set(FIBER_PROTOC_TARGET "protoc" CACHE STRING "protoc target used by fiber_proto_library()")
    else()
        message(FATAL_ERROR "protobuf was fetched but no protoc target was found.")
    endif()
endfunction()
