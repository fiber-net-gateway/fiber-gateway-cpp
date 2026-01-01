cmake_minimum_required(VERSION 4.1)

set(FIBER_CLANG_BIN "/usr/bin")
set(FIBER_LLVM_ROOT "/usr/lib/llvm-21")
if (NOT DEFINED CMAKE_C_COMPILER AND EXISTS "${FIBER_CLANG_BIN}/clang-21")
    set(CMAKE_C_COMPILER "${FIBER_CLANG_BIN}/clang-21" CACHE FILEPATH "Default clang C compiler")
endif()
if (NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "${FIBER_CLANG_BIN}/clang++-21")
    set(CMAKE_CXX_COMPILER "${FIBER_CLANG_BIN}/clang++-21" CACHE FILEPATH "Default clang C++ compiler")
endif()
set(FIBER_CLANG_LIBDIR "${FIBER_LLVM_ROOT}/lib")
set(FIBER_CLANG_INCLUDEDIR "${FIBER_LLVM_ROOT}/include/c++/v1")
set(FIBER_USE_LIBCXX OFF)
if (CMAKE_CXX_COMPILER STREQUAL "${FIBER_CLANG_BIN}/clang++-21"
    AND EXISTS "${FIBER_CLANG_LIBDIR}"
    AND EXISTS "${FIBER_CLANG_INCLUDEDIR}")
    set(FIBER_USE_LIBCXX ON)
    set(CMAKE_CXX_FLAGS_INIT
        "${CMAKE_CXX_FLAGS_INIT} -stdlib=libc++ -isystem ${FIBER_CLANG_INCLUDEDIR}")
    set(FIBER_STDLIB_LINK_FLAGS -stdlib=libc++ -L${FIBER_CLANG_LIBDIR} -lc++abi -Wl,-rpath,${FIBER_CLANG_LIBDIR})
endif()

project(fiber)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GLOB_RECURSE CPP_SRC_LIST ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
set(MAIN_SRC ${CMAKE_CURRENT_LIST_DIR}/src/main.cpp)
list(REMOVE_ITEM CPP_SRC_LIST ${MAIN_SRC})

add_library(fiber_lib ${CPP_SRC_LIST})
target_include_directories(fiber_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)
if (FIBER_USE_LIBCXX)
    target_link_options(fiber_lib INTERFACE ${FIBER_STDLIB_LINK_FLAGS})
endif()

add_executable(fiber ${MAIN_SRC})
target_link_libraries(fiber PRIVATE fiber_lib)

option(FIBER_BUILD_TESTS "Build tests" ON)
if(FIBER_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if (TARGET gtest)
            target_compile_options(gtest PRIVATE -Wno-character-conversion)
        endif()
        if (TARGET gtest_main)
            target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
        endif()
    endif()

    enable_testing()
    add_executable(fiber_tests
        tests/ArrayTest.cpp
        tests/SanityTest.cpp
        tests/GeneratorTest.cpp
        tests/IteratorTest.cpp
        tests/JsValueOpsTest.cpp
        tests/JsValueEncodeTest.cpp
        tests/ParserTest.cpp
        tests/ScriptParserTest.cpp
        tests/ScriptCompilerTest.cpp
        tests/ScriptRuntimeOpsTest.cpp
        tests/ScriptExecutionTest.cpp
    )
    target_link_libraries(fiber_tests PRIVATE fiber_lib GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(fiber_tests)
endif()
