include_guard()

include(ExternalProject)
include(FetchContent)

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

    set(FIBER_JEMALLOC_VERSION "5.3.0")
    set(FIBER_JEMALLOC_SOURCE_DIR "${FETCHCONTENT_BASE_DIR}/jemalloc-src")
    set(FIBER_JEMALLOC_BINARY_DIR "${CMAKE_BINARY_DIR}/_deps/jemalloc-build")
    set(FIBER_JEMALLOC_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/jemalloc-install")

    ExternalProject_Add(
        fiber_jemalloc_ep
        URL "https://github.com/jemalloc/jemalloc/releases/download/${FIBER_JEMALLOC_VERSION}/jemalloc-${FIBER_JEMALLOC_VERSION}.tar.bz2"
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
    URL https://github.com/google/boringssl/archive/refs/tags/0.20251124.0.tar.gz
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
    URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
    SOURCE_SUBDIR fiber_download_only
)
FetchContent_MakeAvailable(zlib)
set(FIBER_ZLIB_SOURCE_DIR "${zlib_SOURCE_DIR}" CACHE PATH "Downloaded zlib source directory" FORCE)

# ---- protobuf-lite runtime + protoc codegen ----
# v21.12 is the last release before protobuf made abseil-cpp a hard dependency
# (22.x+). Staying on 3.21 keeps the fetched dependency tree small (no abseil /
# utf8_range); libprotobuf-lite + protoc suffice for gRPC message encode/decode.
# v21.12 declares cmake_minimum_required(VERSION 3.5), which CMake 4.x still
# accepts (only < 3.5 was dropped).
#
# protobuf is configured here, before the top-level sets CMAKE_CXX_STANDARD 23,
# so its targets build under the compiler's default standard (C++17) rather than
# C++23 - this sidesteps C++20/23 removals (e.g. std::iterator) in 3.21.
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
fiber_use_cached_content(protobuf)
FetchContent_Declare(
    protobuf
    URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v21.12.tar.gz
)
FetchContent_MakeAvailable(protobuf)

# Ensure the protobuf:: aliases exist (3.21 may or may not create them).
if(NOT TARGET protobuf::libprotobuf-lite AND TARGET libprotobuf-lite)
    add_library(protobuf::libprotobuf-lite ALIAS libprotobuf-lite)
endif()
# protoc is an executable; aliasing it is unreliable across CMake versions, so
# capture the real target name for the codegen helper.
if(TARGET protobuf::protoc)
    set(FIBER_PROTOC_TARGET "protobuf::protoc" CACHE STRING "protoc target used by fiber_proto_library()")
elseif(TARGET protoc)
    set(FIBER_PROTOC_TARGET "protoc" CACHE STRING "protoc target used by fiber_proto_library()")
else()
    message(FATAL_ERROR "protobuf was fetched but no protoc target was found.")
endif()
