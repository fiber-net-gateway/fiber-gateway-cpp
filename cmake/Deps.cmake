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

function(fiber_lsquic_auto_asan_enabled output_var)
    set(fiber_lsquic_needs_asan FALSE)
    if ((CMAKE_BUILD_TYPE STREQUAL "" OR CMAKE_BUILD_TYPE STREQUAL "Debug")
        AND CMAKE_C_COMPILER MATCHES "clang"
        AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android"
        AND NOT "$ENV{TRAVIS}" MATCHES "^true$"
        AND NOT "$ENV{EXTRA_CFLAGS}" MATCHES "-fsanitize")
        set(fiber_lsquic_needs_asan TRUE)
    endif()
    set(${output_var} ${fiber_lsquic_needs_asan} PARENT_SCOPE)
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
set(LSQUIC_BIN OFF CACHE BOOL "" FORCE)
set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
set(LSQUIC_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LSQUIC_DEVEL OFF CACHE BOOL "" FORCE)
set(LSQUIC_WEBTRANSPORT OFF CACHE BOOL "" FORCE)
set(LSQUIC_LIBSSL BORINGSSL CACHE STRING "" FORCE)
set(LIBSSL_DIR "${boringssl_SOURCE_DIR}" CACHE PATH "" FORCE)
set(LIBSSL_LIB "${boringssl_BINARY_DIR}" CACHE PATH "" FORCE)
set(LIBSSL_LIB_ssl ssl CACHE STRING "" FORCE)
set(LIBSSL_LIB_crypto crypto CACHE STRING "" FORCE)

set(FIBER_ZLIB_TARGET "")
find_package(ZLIB QUIET)
if (TARGET ZLIB::ZLIB)
    set(FIBER_ZLIB_TARGET ZLIB::ZLIB)
endif()

if (NOT FIBER_ZLIB_TARGET AND FIBER_FETCH_DEPS)
    fiber_use_cached_content(zlib)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_FILES ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        zlib
        URL https://github.com/madler/zlib/archive/refs/tags/v1.3.2.tar.gz
    )
    FetchContent_MakeAvailable(zlib)

    if (TARGET zlibstatic)
        if (NOT TARGET fiber_zlib)
            add_library(fiber_zlib INTERFACE)
            target_link_libraries(fiber_zlib INTERFACE zlibstatic)
            target_include_directories(fiber_zlib INTERFACE
                "${zlib_SOURCE_DIR}"
                "${zlib_BINARY_DIR}")
        endif()
        if (NOT TARGET ZLIB::ZLIB)
            add_library(ZLIB::ZLIB ALIAS fiber_zlib)
        endif()
        set(FIBER_ZLIB_TARGET ZLIB::ZLIB)

        # lsquic's build only accepts one include directory variable. Stage the
        # generated zconf.h and the source zlib.h into a single directory first.
        set(FIBER_ZLIB_INCLUDE_DIR "${zlib_BINARY_DIR}/fiber-zlib-include")
        file(MAKE_DIRECTORY "${FIBER_ZLIB_INCLUDE_DIR}")
        file(COPY_FILE "${zlib_SOURCE_DIR}/zlib.h" "${FIBER_ZLIB_INCLUDE_DIR}/zlib.h" ONLY_IF_DIFFERENT)
        file(COPY_FILE "${zlib_BINARY_DIR}/zconf.h" "${FIBER_ZLIB_INCLUDE_DIR}/zconf.h" ONLY_IF_DIFFERENT)
        set(ZLIB_INCLUDE_DIR "${FIBER_ZLIB_INCLUDE_DIR}" CACHE PATH "" FORCE)
        set(ZLIB_LIB zlibstatic CACHE STRING "" FORCE)
    endif()
endif()

if (FIBER_ZLIB_TARGET AND NOT DEFINED ZLIB_INCLUDE_DIR)
    if (DEFINED ZLIB_INCLUDE_DIRS)
        list(GET ZLIB_INCLUDE_DIRS 0 ZLIB_INCLUDE_DIR)
        set(ZLIB_INCLUDE_DIR "${ZLIB_INCLUDE_DIR}" CACHE PATH "" FORCE)
    endif()
endif()

if (FIBER_ZLIB_TARGET AND NOT DEFINED ZLIB_LIB)
    if (DEFINED ZLIB_LIBRARY)
        set(ZLIB_LIB "${ZLIB_LIBRARY}" CACHE FILEPATH "" FORCE)
    elseif (DEFINED ZLIB_LIBRARIES)
        list(GET ZLIB_LIBRARIES 0 ZLIB_LIB)
        set(ZLIB_LIB "${ZLIB_LIB}" CACHE FILEPATH "" FORCE)
    endif()
endif()

set(FIBER_HAVE_LSQUIC OFF)
if (NOT FIBER_ENABLE_HTTP3)
    message(STATUS "Skipping lsquic dependency because FIBER_ENABLE_HTTP3=OFF.")
elseif (FIBER_ZLIB_TARGET AND DEFINED ZLIB_INCLUDE_DIR AND DEFINED ZLIB_LIB)
    fiber_use_cached_content(lsquic)
    FetchContent_Declare(
        lsquic
        GIT_REPOSITORY https://github.com/litespeedtech/lsquic.git
        GIT_TAG v4.5.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lsquic)

    fiber_lsquic_auto_asan_enabled(FIBER_LSQUIC_AUTO_ASAN)
    if (FIBER_LSQUIC_AUTO_ASAN)
        if (TARGET lsquic)
            # lsquic enables ASan internally in Debug Clang builds but does not
            # propagate the runtime requirement to consumers of the static lib.
            target_link_options(lsquic INTERFACE -fsanitize=address)
        elseif (TARGET lsquic_static)
            target_link_options(lsquic_static INTERFACE -fsanitize=address)
        endif()
    endif()

    if (NOT TARGET lsquic::lsquic)
        if (TARGET lsquic)
            get_target_property(_lsquic_real lsquic ALIASED_TARGET)
            if (_lsquic_real)
                add_library(lsquic::lsquic ALIAS ${_lsquic_real})
            else()
                add_library(lsquic::lsquic ALIAS lsquic)
            endif()
        elseif (TARGET lsquic_static)
            add_library(lsquic::lsquic ALIAS lsquic_static)
        endif()
    endif()

    if (TARGET lsquic::lsquic)
        set(FIBER_HAVE_LSQUIC ON)
    endif()
else()
    message(STATUS
        "Skipping lsquic dependency because zlib was not found. "
        "Install zlib development files or enable FIBER_FETCH_DEPS=ON to download it automatically.")
endif()
