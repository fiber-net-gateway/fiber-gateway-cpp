include(CheckCXXSourceCompiles)

function(fiber_validate_cxx_toolchain)
    if (NOT DEFINED FIBER_MIN_GCC_VERSION)
        set(FIBER_MIN_GCC_VERSION 13.0)
    endif()
    if (NOT DEFINED FIBER_MIN_CLANG_VERSION)
        set(FIBER_MIN_CLANG_VERSION 17.0)
    endif()

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS FIBER_MIN_GCC_VERSION)
            message(FATAL_ERROR
                "C++23 requires GCC ${FIBER_MIN_GCC_VERSION}+ for this project, "
                "but found GCC ${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS FIBER_MIN_CLANG_VERSION)
            message(FATAL_ERROR
                "C++23 requires Clang ${FIBER_MIN_CLANG_VERSION}+ for this project, "
                "but found Clang ${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
    endif()

    set(FIBER_CXX23_CHECK_OLD_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(FIBER_CXX23_CHECK_OLD_REQUIRED_LINK_OPTIONS "${CMAKE_REQUIRED_LINK_OPTIONS}")
    if (FIBER_USE_LIBCXX)
        string(APPEND CMAKE_REQUIRED_FLAGS " -stdlib=libc++ -isystem ${FIBER_CLANG_INCLUDEDIR}")
        list(APPEND CMAKE_REQUIRED_LINK_OPTIONS
            -stdlib=libc++
            "-L${FIBER_CLANG_LIBDIR}"
            -lc++abi
            "-Wl,-rpath,${FIBER_CLANG_LIBDIR}")
    endif()

    unset(FIBER_USING_LIBCXX CACHE)
    unset(FIBER_USING_LIBSTDCXX CACHE)
    unset(FIBER_HAS_CXX23_EXPECTED CACHE)

    check_cxx_source_compiles([=[
        #include <string>
        #if !defined(_LIBCPP_VERSION)
        #error "libc++ not detected"
        #endif
        int main() {
            return 0;
        }
    ]=] FIBER_USING_LIBCXX)

    check_cxx_source_compiles([=[
        #include <string>
        #if !defined(__GLIBCXX__)
        #error "libstdc++ not detected"
        #endif
        int main() {
            return 0;
        }
    ]=] FIBER_USING_LIBSTDCXX)

    set(FIBER_STDLIB_FAMILY "unknown")
    if (FIBER_USING_LIBCXX)
        set(FIBER_STDLIB_FAMILY "libc++")
    elseif (FIBER_USING_LIBSTDCXX)
        set(FIBER_STDLIB_FAMILY "libstdc++")
    endif()

    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if (FIBER_USE_LIBCXX)
            message(FATAL_ERROR "GCC builds must use libstdc++; libc++ is only supported with Clang.")
        endif()
        if (NOT FIBER_STDLIB_FAMILY STREQUAL "libstdc++")
            message(FATAL_ERROR
                "GCC ${CMAKE_CXX_COMPILER_VERSION} must be paired with libstdc++, "
                "but detected ${FIBER_STDLIB_FAMILY}.")
        endif()
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if (FIBER_USE_LIBCXX)
            if (NOT FIBER_STDLIB_FAMILY STREQUAL "libc++")
                message(FATAL_ERROR
                    "Clang was configured for libc++, but the active standard library "
                    "does not match.")
            endif()
        else()
            if (NOT FIBER_STDLIB_FAMILY STREQUAL "libstdc++")
                message(FATAL_ERROR
                    "Clang without FIBER_USE_LIBCXX must be paired with libstdc++, "
                    "but detected ${FIBER_STDLIB_FAMILY}.")
            endif()
        endif()
    endif()

    message(STATUS "Using C++ standard library: ${FIBER_STDLIB_FAMILY}")

    check_cxx_source_compiles([=[
        #include <expected>
        int main() {
            std::expected<int, int> value{42};
            return value ? 0 : 1;
        }
    ]=] FIBER_HAS_CXX23_EXPECTED)

    set(CMAKE_REQUIRED_FLAGS "${FIBER_CXX23_CHECK_OLD_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${FIBER_CXX23_CHECK_OLD_REQUIRED_LINK_OPTIONS}")

    if (NOT FIBER_HAS_CXX23_EXPECTED)
        message(FATAL_ERROR
            "The selected C++ toolchain does not provide the C++23 standard library "
            "support required by this project (std::expected).")
    endif()
endfunction()
