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
    set(FIBER_CXX23_CHECK_OLD_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
    unset(FIBER_HAS_CXX23_EXPECTED CACHE)
    if (FIBER_USE_LIBCXX)
        string(APPEND CMAKE_REQUIRED_FLAGS " -stdlib=libc++ -isystem ${FIBER_CLANG_INCLUDEDIR}")
        list(APPEND CMAKE_REQUIRED_LINK_OPTIONS ${FIBER_STDLIB_LINK_OPTIONS})
        list(APPEND CMAKE_REQUIRED_LIBRARIES ${FIBER_STDLIB_LINK_LIBRARIES})
    endif()

    check_cxx_source_compiles([=[
        #include <expected>
        int main() {
            std::expected<int, int> value{42};
            return value ? 0 : 1;
        }
    ]=] FIBER_HAS_CXX23_EXPECTED)

    set(CMAKE_REQUIRED_FLAGS "${FIBER_CXX23_CHECK_OLD_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_LINK_OPTIONS "${FIBER_CXX23_CHECK_OLD_REQUIRED_LINK_OPTIONS}")
    set(CMAKE_REQUIRED_LIBRARIES "${FIBER_CXX23_CHECK_OLD_REQUIRED_LIBRARIES}")

    if (NOT FIBER_HAS_CXX23_EXPECTED)
        message(FATAL_ERROR
            "The selected C++ toolchain does not provide the C++23 standard library "
            "support required by this project (std::expected).\n"
            "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
            "(${CMAKE_CXX_COMPILER}).\n"
            "On Linux, Clang usually uses the system libstdc++ by default. "
            "If that standard library is too old, install a newer GCC/libstdc++ "
            "(GCC 13+ recommended) or explicitly configure a Clang+libc++ toolchain.")
    endif()
endfunction()
