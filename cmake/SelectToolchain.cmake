if (NOT DEFINED FIBER_MIN_GCC_VERSION)
    set(FIBER_MIN_GCC_VERSION 13.0)
endif()
if (NOT DEFINED FIBER_MIN_CLANG_VERSION)
    set(FIBER_MIN_CLANG_VERSION 17.0)
endif()
if (NOT DEFINED FIBER_USE_LIBCXX)
    set(FIBER_USE_LIBCXX OFF CACHE BOOL "Use libc++ with Clang toolchains")
endif()
if (NOT DEFINED FIBER_STATIC_LIBCXX)
    set(FIBER_STATIC_LIBCXX ON CACHE BOOL "Statically link libc++ runtime libraries when FIBER_USE_LIBCXX=ON")
endif()

function(_fiber_compiler_get_version compiler_path output_var)
    execute_process(
        COMMAND "${compiler_path}" -dumpfullversion -dumpversion
        RESULT_VARIABLE fiber_compiler_version_result
        OUTPUT_VARIABLE fiber_compiler_version
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (fiber_compiler_version_result EQUAL 0 AND NOT fiber_compiler_version STREQUAL "")
        set(${output_var} "${fiber_compiler_version}" PARENT_SCOPE)
    else()
        set(${output_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(_fiber_compiler_is_clang compiler_path output_var)
    execute_process(
        COMMAND "${compiler_path}" --version
        RESULT_VARIABLE fiber_compiler_id_result
        OUTPUT_VARIABLE fiber_compiler_id_output
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (fiber_compiler_id_result EQUAL 0
        AND (fiber_compiler_id_output MATCHES "(^|.*[[:space:]])clang version[[:space:]].*"
            OR fiber_compiler_id_output MATCHES "(^|.*[[:space:]])Apple clang version[[:space:]].*"
            OR fiber_compiler_id_output MATCHES "(^|.*[[:space:]])Ubuntu clang version[[:space:]].*"))
        set(${output_var} TRUE PARENT_SCOPE)
    else()
        set(fiber_clang_macro_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/fiber-toolchain-probe")
        set(fiber_clang_macro_probe_src "${fiber_clang_macro_probe_dir}/clang-probe.cpp")
        file(MAKE_DIRECTORY "${fiber_clang_macro_probe_dir}")
        file(WRITE "${fiber_clang_macro_probe_src}" "int main() { return 0; }\n")
        execute_process(
            COMMAND "${compiler_path}" -dM -E -x c++ "${fiber_clang_macro_probe_src}"
            RESULT_VARIABLE fiber_clang_macro_result
            OUTPUT_VARIABLE fiber_clang_macro_output
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (fiber_clang_macro_result EQUAL 0
            AND fiber_clang_macro_output MATCHES "(^|[\r\n])#define __clang__ 1([\r\n]|$)")
            set(${output_var} TRUE PARENT_SCOPE)
        else()
            set(${output_var} FALSE PARENT_SCOPE)
        endif()
    endif()
endfunction()

function(_fiber_find_clang_libcxx_include_dir clang_compiler_path output_var)
    set(fiber_clang_include_dir "")
    _fiber_compiler_get_version("${clang_compiler_path}" fiber_clang_version)
    if (NOT fiber_clang_version STREQUAL "")
        string(REGEX MATCH "^[0-9]+" fiber_clang_major "${fiber_clang_version}")
        foreach(fiber_include_candidate
            "/usr/lib/llvm-${fiber_clang_major}/include/c++/v1"
            "/usr/include/c++/v1")
            if (EXISTS "${fiber_include_candidate}")
                set(fiber_clang_include_dir "${fiber_include_candidate}")
                break()
            endif()
        endforeach()
    endif()
    set(${output_var} "${fiber_clang_include_dir}" PARENT_SCOPE)
endfunction()

function(_fiber_find_clang_library_dirs clang_compiler_path libcxx_include_dir output_var)
    set(fiber_library_dirs "")

    _fiber_compiler_get_version("${clang_compiler_path}" fiber_clang_version)
    if (NOT fiber_clang_version STREQUAL "")
        string(REGEX MATCH "^[0-9]+" fiber_clang_major "${fiber_clang_version}")
        foreach(fiber_library_candidate
            "/usr/lib/llvm-${fiber_clang_major}/lib"
            "/usr/lib/llvm-${fiber_clang_major}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
            "/usr/local/lib/llvm-${fiber_clang_major}/lib")
            if (EXISTS "${fiber_library_candidate}")
                list(APPEND fiber_library_dirs "${fiber_library_candidate}")
            endif()
        endforeach()
    endif()

    if (NOT libcxx_include_dir STREQUAL "")
        get_filename_component(fiber_libcxx_include_parent "${libcxx_include_dir}" DIRECTORY)
        get_filename_component(fiber_libcxx_root "${fiber_libcxx_include_parent}" DIRECTORY)
        get_filename_component(fiber_libcxx_root "${fiber_libcxx_root}" DIRECTORY)
        foreach(fiber_library_candidate
            "${fiber_libcxx_root}/lib"
            "${fiber_libcxx_root}/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
            "${fiber_libcxx_root}/lib64")
            if (EXISTS "${fiber_library_candidate}")
                list(APPEND fiber_library_dirs "${fiber_library_candidate}")
            endif()
        endforeach()
    endif()

    foreach(fiber_library_candidate
        "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
        "/usr/local/lib/${CMAKE_LIBRARY_ARCHITECTURE}"
        "/usr/lib/x86_64-linux-gnu"
        "/usr/local/lib/x86_64-linux-gnu"
        "/usr/lib64"
        "/usr/local/lib64"
        "/usr/lib"
        "/usr/local/lib")
        if (EXISTS "${fiber_library_candidate}")
            list(APPEND fiber_library_dirs "${fiber_library_candidate}")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES fiber_library_dirs)
    set(${output_var} "${fiber_library_dirs}" PARENT_SCOPE)
endfunction()

function(_fiber_find_static_library output_var library_name)
    set(options)
    set(one_value_args)
    set(multi_value_args HINTS)
    cmake_parse_arguments(FIBER_STATIC_LIB "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(fiber_saved_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    find_library(fiber_static_library
        NAMES "${library_name}"
        HINTS ${FIBER_STATIC_LIB_HINTS}
        NO_CACHE)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${fiber_saved_library_suffixes}")

    set(${output_var} "${fiber_static_library}" PARENT_SCOPE)
endfunction()

function(_fiber_compiler_supports_cxx23 compiler_path output_var)
    set(fiber_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/fiber-toolchain-probe")
    set(fiber_probe_src "${fiber_probe_dir}/probe.cpp")
    set(fiber_probe_obj "${fiber_probe_dir}/probe.o")

    file(MAKE_DIRECTORY "${fiber_probe_dir}")
    file(WRITE "${fiber_probe_src}" [=[
#include <expected>

int main() {
    std::expected<int, int> value{42};
    return value ? 0 : 1;
}
]=])

    _fiber_compiler_is_clang("${compiler_path}" fiber_probe_is_clang)
    if (FIBER_USE_LIBCXX AND fiber_probe_is_clang)
        _fiber_find_clang_libcxx_include_dir("${compiler_path}" fiber_probe_libcxx_include_dir)
        if (fiber_probe_libcxx_include_dir STREQUAL "")
            set(${output_var} FALSE PARENT_SCOPE)
            return()
        endif()
        execute_process(
            COMMAND
                "${compiler_path}"
                -std=c++23
                -stdlib=libc++
                -isystem "${fiber_probe_libcxx_include_dir}"
                -x c++
                -c "${fiber_probe_src}"
                -o "${fiber_probe_obj}"
            RESULT_VARIABLE fiber_probe_result
            OUTPUT_QUIET
            ERROR_QUIET)
    else()
        execute_process(
            COMMAND "${compiler_path}" -std=c++23 -x c++ -c "${fiber_probe_src}" -o "${fiber_probe_obj}"
            RESULT_VARIABLE fiber_probe_result
            OUTPUT_QUIET
            ERROR_QUIET)
    endif()

    if (fiber_probe_result EQUAL 0)
        set(${output_var} TRUE PARENT_SCOPE)
    else()
        set(${output_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_fiber_find_best_cxx_compiler compiler_family output_path_var output_version_var)
    if (compiler_family STREQUAL "clang")
        set(fiber_min_version "${FIBER_MIN_CLANG_VERSION}")
        set(fiber_candidate_names "clang++")
        foreach(fiber_version RANGE 17 40)
            list(APPEND fiber_candidate_names "clang++-${fiber_version}")
        endforeach()
    elseif (compiler_family STREQUAL "gcc")
        set(fiber_min_version "${FIBER_MIN_GCC_VERSION}")
        set(fiber_candidate_names "g++")
        foreach(fiber_version RANGE 13 40)
            list(APPEND fiber_candidate_names "g++-${fiber_version}")
        endforeach()
    else()
        message(FATAL_ERROR "Unsupported compiler family: ${compiler_family}")
    endif()

    set(fiber_best_compiler "")
    set(fiber_best_version "")
    set(fiber_seen_paths "")
    foreach(fiber_candidate_name IN LISTS fiber_candidate_names)
        find_program(fiber_candidate_path NAMES "${fiber_candidate_name}" NO_CACHE)
        if (NOT fiber_candidate_path)
            continue()
        endif()

        get_filename_component(fiber_candidate_realpath "${fiber_candidate_path}" REALPATH)
        list(FIND fiber_seen_paths "${fiber_candidate_realpath}" fiber_seen_index)
        if (NOT fiber_seen_index EQUAL -1)
            continue()
        endif()
        list(APPEND fiber_seen_paths "${fiber_candidate_realpath}")

        _fiber_compiler_get_version("${fiber_candidate_path}" fiber_candidate_version)
        if (fiber_candidate_version STREQUAL ""
            OR fiber_candidate_version VERSION_LESS fiber_min_version)
            continue()
        endif()

        _fiber_compiler_supports_cxx23("${fiber_candidate_path}" fiber_candidate_supports_cxx23)
        if (NOT fiber_candidate_supports_cxx23)
            continue()
        endif()

        if (fiber_best_version STREQUAL ""
            OR fiber_candidate_version VERSION_GREATER fiber_best_version)
            set(fiber_best_compiler "${fiber_candidate_path}")
            set(fiber_best_version "${fiber_candidate_version}")
        endif()
    endforeach()

    set(${output_path_var} "${fiber_best_compiler}" PARENT_SCOPE)
    set(${output_version_var} "${fiber_best_version}" PARENT_SCOPE)
endfunction()

function(_fiber_find_matching_c_compiler cxx_compiler_path output_var)
    get_filename_component(fiber_cxx_compiler_name "${cxx_compiler_path}" NAME)
    set(fiber_c_candidate_names "")

    if (fiber_cxx_compiler_name MATCHES "^clang\\+\\+-([0-9]+)$")
        list(APPEND fiber_c_candidate_names "clang-${CMAKE_MATCH_1}")
    elseif (fiber_cxx_compiler_name MATCHES "^g\\+\\+-([0-9]+)$")
        list(APPEND fiber_c_candidate_names "gcc-${CMAKE_MATCH_1}")
    endif()

    if (fiber_cxx_compiler_name MATCHES "^clang\\+\\+")
        list(APPEND fiber_c_candidate_names "clang")
    elseif (fiber_cxx_compiler_name MATCHES "^g\\+\\+")
        list(APPEND fiber_c_candidate_names "gcc")
    endif()

    foreach(fiber_c_candidate_name IN LISTS fiber_c_candidate_names)
        find_program(fiber_c_candidate_path NAMES "${fiber_c_candidate_name}" NO_CACHE)
        if (fiber_c_candidate_path)
            set(${output_var} "${fiber_c_candidate_path}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${output_var} "" PARENT_SCOPE)
endfunction()

macro(_fiber_select_default_toolchain)
    if (NOT DEFINED CMAKE_CXX_COMPILER
        AND NOT ((DEFINED ENV{CXX} AND NOT "$ENV{CXX}" STREQUAL "")
            OR (DEFINED ENV{CC} AND NOT "$ENV{CC}" STREQUAL "")
            OR DEFINED CMAKE_TOOLCHAIN_FILE))
        _fiber_find_best_cxx_compiler("clang" fiber_selected_cxx_compiler fiber_selected_cxx_version)
        if (fiber_selected_cxx_compiler STREQUAL "" AND NOT FIBER_USE_LIBCXX)
            _fiber_find_best_cxx_compiler("gcc" fiber_selected_cxx_compiler fiber_selected_cxx_version)
        endif()

        if (fiber_selected_cxx_compiler STREQUAL "")
            if (FIBER_USE_LIBCXX)
                message(FATAL_ERROR
                    "FIBER_USE_LIBCXX=ON requires a Clang toolchain with libc++ headers available, "
                    "but no supported Clang C++23 compiler was auto-detected.")
            else()
                message(STATUS "No supported Clang/GCC C++23 compiler auto-detected; letting CMake resolve the toolchain.")
            endif()
        else()
            set(CMAKE_CXX_COMPILER "${fiber_selected_cxx_compiler}")
            set(CMAKE_CXX_COMPILER "${fiber_selected_cxx_compiler}" CACHE FILEPATH "Default detected C++ compiler")
            message(STATUS "Selected default C++ compiler: ${fiber_selected_cxx_compiler} (${fiber_selected_cxx_version})")

            if (NOT DEFINED CMAKE_C_COMPILER)
                _fiber_find_matching_c_compiler("${fiber_selected_cxx_compiler}" fiber_selected_c_compiler)
                if (NOT fiber_selected_c_compiler STREQUAL "")
                    set(CMAKE_C_COMPILER "${fiber_selected_c_compiler}")
                    set(CMAKE_C_COMPILER "${fiber_selected_c_compiler}" CACHE FILEPATH "Matching C compiler for selected C++ compiler")
                endif()
            endif()
        endif()
    endif()
endmacro()

macro(_fiber_configure_clang_stdlib)
    set(FIBER_STDLIB_LINK_OPTIONS "")
    set(FIBER_STDLIB_LINK_LIBRARIES "")
    if (FIBER_USE_LIBCXX)
        if (NOT DEFINED CMAKE_CXX_COMPILER)
            message(FATAL_ERROR "FIBER_USE_LIBCXX=ON requires a configured C++ compiler.")
        endif()

        _fiber_compiler_is_clang("${CMAKE_CXX_COMPILER}" fiber_cxx_compiler_is_clang)
        if (NOT fiber_cxx_compiler_is_clang)
            message(FATAL_ERROR "FIBER_USE_LIBCXX=ON requires Clang, but selected compiler is ${CMAKE_CXX_COMPILER}.")
        endif()

        _fiber_find_clang_libcxx_include_dir("${CMAKE_CXX_COMPILER}" fiber_selected_clang_includedir)
        if (fiber_selected_clang_includedir STREQUAL "")
            message(FATAL_ERROR
                "FIBER_USE_LIBCXX=ON was requested, but libc++ headers were not found. "
                "Install libc++ development headers such as /usr/include/c++/v1.")
        endif()

        set(FIBER_CLANG_INCLUDEDIR "${fiber_selected_clang_includedir}")
        set(CMAKE_CXX_FLAGS_INIT
            "${CMAKE_CXX_FLAGS_INIT} -stdlib=libc++ -isystem ${fiber_selected_clang_includedir}"
        )
        list(APPEND FIBER_STDLIB_LINK_OPTIONS -stdlib=libc++)

        if (FIBER_STATIC_LIBCXX)
            if (APPLE)
                message(FATAL_ERROR
                    "FIBER_STATIC_LIBCXX=ON is not supported on Apple toolchains. "
                    "Set FIBER_STATIC_LIBCXX=OFF to use the system libc++ dylib.")
            endif()

            # Suppress Clang's implicit dynamic -lc++ after the manually selected
            # static runtime archives below.
            list(APPEND FIBER_STDLIB_LINK_OPTIONS -nostdlib++)
            _fiber_find_clang_library_dirs("${CMAKE_CXX_COMPILER}" "${fiber_selected_clang_includedir}" fiber_clang_library_dirs)
            _fiber_find_static_library(fiber_libcxx_static c++ HINTS ${fiber_clang_library_dirs})
            _fiber_find_static_library(fiber_libcxxabi_static c++abi HINTS ${fiber_clang_library_dirs})
            _fiber_find_static_library(fiber_libunwind_static unwind HINTS ${fiber_clang_library_dirs})

            if (fiber_libcxx_static STREQUAL "" OR fiber_libcxxabi_static STREQUAL "")
                message(FATAL_ERROR
                    "FIBER_USE_LIBCXX=ON defaults to static libc++ linking, but required static archives were not found.\n"
                    "Missing libc++ archive: ${fiber_libcxx_static}\n"
                    "Missing libc++abi archive: ${fiber_libcxxabi_static}\n"
                    "Searched library directories: ${fiber_clang_library_dirs}\n"
                    "Install static libc++/libc++abi packages or set FIBER_STATIC_LIBCXX=OFF to use shared libc++.")
            endif()

            list(APPEND FIBER_STDLIB_LINK_LIBRARIES
                -Wl,-Bstatic
                "${fiber_libcxx_static}"
                "${fiber_libcxxabi_static}")
            if (NOT fiber_libunwind_static STREQUAL "")
                list(APPEND FIBER_STDLIB_LINK_LIBRARIES "${fiber_libunwind_static}")
            endif()
            list(APPEND FIBER_STDLIB_LINK_LIBRARIES -Wl,-Bdynamic)
        endif()
    endif()
endmacro()

_fiber_select_default_toolchain()
_fiber_configure_clang_stdlib()
