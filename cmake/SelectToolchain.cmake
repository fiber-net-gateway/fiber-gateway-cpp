if (NOT DEFINED FIBER_MIN_GCC_VERSION)
    set(FIBER_MIN_GCC_VERSION 13.0)
endif()
if (NOT DEFINED FIBER_MIN_CLANG_VERSION)
    set(FIBER_MIN_CLANG_VERSION 17.0)
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

    execute_process(
        COMMAND "${compiler_path}" -std=c++23 -x c++ -c "${fiber_probe_src}" -o "${fiber_probe_obj}"
        RESULT_VARIABLE fiber_probe_result
        OUTPUT_QUIET
        ERROR_QUIET)
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

function(_fiber_select_default_toolchain)
    if (DEFINED CMAKE_CXX_COMPILER)
        return()
    endif()
    if ((DEFINED ENV{CXX} AND NOT "$ENV{CXX}" STREQUAL "")
        OR (DEFINED ENV{CC} AND NOT "$ENV{CC}" STREQUAL "")
        OR DEFINED CMAKE_TOOLCHAIN_FILE)
        return()
    endif()

    _fiber_find_best_cxx_compiler("clang" fiber_selected_cxx_compiler fiber_selected_cxx_version)
    if (fiber_selected_cxx_compiler STREQUAL "")
        _fiber_find_best_cxx_compiler("gcc" fiber_selected_cxx_compiler fiber_selected_cxx_version)
    endif()

    if (fiber_selected_cxx_compiler STREQUAL "")
        message(STATUS "No supported Clang/GCC C++23 compiler auto-detected; letting CMake resolve the toolchain.")
        return()
    endif()

    set(CMAKE_CXX_COMPILER "${fiber_selected_cxx_compiler}" CACHE FILEPATH "Default detected C++ compiler")
    message(STATUS "Selected default C++ compiler: ${fiber_selected_cxx_compiler} (${fiber_selected_cxx_version})")

    if (NOT DEFINED CMAKE_C_COMPILER)
        _fiber_find_matching_c_compiler("${fiber_selected_cxx_compiler}" fiber_selected_c_compiler)
        if (NOT fiber_selected_c_compiler STREQUAL "")
            set(CMAKE_C_COMPILER "${fiber_selected_c_compiler}" CACHE FILEPATH "Matching C compiler for selected C++ compiler")
        endif()
    endif()
endfunction()

function(_fiber_configure_clang_stdlib)
    set(FIBER_USE_LIBCXX OFF PARENT_SCOPE)
    set(FIBER_STDLIB_LINK_FLAGS "" PARENT_SCOPE)

    if (NOT DEFINED CMAKE_CXX_COMPILER)
        return()
    endif()

    get_filename_component(fiber_cxx_compiler_name "${CMAKE_CXX_COMPILER}" NAME)
    if (NOT fiber_cxx_compiler_name MATCHES "^clang\\+\\+")
        return()
    endif()

    _fiber_compiler_get_version("${CMAKE_CXX_COMPILER}" fiber_selected_clang_version)
    if (fiber_selected_clang_version STREQUAL "")
        return()
    endif()

    string(REGEX MATCH "^[0-9]+" fiber_selected_clang_major "${fiber_selected_clang_version}")
    set(FIBER_LLVM_ROOT "/usr/lib/llvm-${fiber_selected_clang_major}" PARENT_SCOPE)
    set(FIBER_CLANG_LIBDIR "/usr/lib/llvm-${fiber_selected_clang_major}/lib" PARENT_SCOPE)
    set(FIBER_CLANG_INCLUDEDIR "/usr/lib/llvm-${fiber_selected_clang_major}/include/c++/v1" PARENT_SCOPE)

    if (EXISTS "/usr/lib/llvm-${fiber_selected_clang_major}/lib"
        AND EXISTS "/usr/lib/llvm-${fiber_selected_clang_major}/include/c++/v1")
        set(FIBER_USE_LIBCXX ON PARENT_SCOPE)
        set(FIBER_STDLIB_LINK_FLAGS
            -stdlib=libc++
            -L/usr/lib/llvm-${fiber_selected_clang_major}/lib
            -lc++abi
            -Wl,-rpath,/usr/lib/llvm-${fiber_selected_clang_major}/lib
            PARENT_SCOPE)
        set(CMAKE_CXX_FLAGS_INIT
            "${CMAKE_CXX_FLAGS_INIT} -stdlib=libc++ -isystem /usr/lib/llvm-${fiber_selected_clang_major}/include/c++/v1"
            PARENT_SCOPE)
    endif()
endfunction()

_fiber_select_default_toolchain()
_fiber_configure_clang_stdlib()
