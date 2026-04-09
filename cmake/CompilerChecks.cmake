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

    unset(FIBER_HAS_CXX23_EXPECTED CACHE)
    set(FIBER_USE_LIBCXX OFF PARENT_SCOPE)
    set(FIBER_STDLIB_LINK_FLAGS "" PARENT_SCOPE)

    check_cxx_source_compiles([=[
        #include <expected>
        int main() {
            std::expected<int, int> value{42};
            return value ? 0 : 1;
        }
    ]=] FIBER_HAS_CXX23_EXPECTED)

    if (NOT FIBER_HAS_CXX23_EXPECTED)
        message(FATAL_ERROR
            "The selected C++ toolchain does not provide the C++23 standard library "
            "support required by this project (std::expected).")
    endif()
endfunction()
