cmake_minimum_required(VERSION 4.1)

set(FIBER_CLANG_ROOT "/home/dear/Desktop/Software/clang")
if (NOT DEFINED CMAKE_C_COMPILER AND EXISTS "${FIBER_CLANG_ROOT}/bin/clang")
    set(CMAKE_C_COMPILER "${FIBER_CLANG_ROOT}/bin/clang" CACHE FILEPATH "Default clang C compiler")
endif()
if (NOT DEFINED CMAKE_CXX_COMPILER AND EXISTS "${FIBER_CLANG_ROOT}/bin/clang++")
    set(CMAKE_CXX_COMPILER "${FIBER_CLANG_ROOT}/bin/clang++" CACHE FILEPATH "Default clang C++ compiler")
endif()
set(FIBER_CLANG_LIBDIR "${FIBER_CLANG_ROOT}/lib/x86_64-unknown-linux-gnu")
set(FIBER_CLANG_INCLUDEDIR "${FIBER_CLANG_ROOT}/include/c++/v1")
if (CMAKE_CXX_COMPILER STREQUAL "${FIBER_CLANG_ROOT}/bin/clang++"
    AND EXISTS "${FIBER_CLANG_LIBDIR}"
    AND EXISTS "${FIBER_CLANG_INCLUDEDIR}")
    set(CMAKE_CXX_FLAGS_INIT
        "${CMAKE_CXX_FLAGS_INIT} -stdlib=libc++ -isystem ${FIBER_CLANG_INCLUDEDIR}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "${CMAKE_EXE_LINKER_FLAGS_INIT} -stdlib=libc++ -L${FIBER_CLANG_LIBDIR} -Wl,-rpath,${FIBER_CLANG_LIBDIR}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT
        "${CMAKE_SHARED_LINKER_FLAGS_INIT} -stdlib=libc++ -L${FIBER_CLANG_LIBDIR} -Wl,-rpath,${FIBER_CLANG_LIBDIR}")
endif()

project(fiber)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GLOB_RECURSE CPP_SRC_LIST ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
set(MAIN_SRC ${CMAKE_CURRENT_LIST_DIR}/src/main.cpp)
list(REMOVE_ITEM CPP_SRC_LIST ${MAIN_SRC})

add_library(fiber_lib ${CPP_SRC_LIST})
target_include_directories(fiber_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src)

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

    enable_testing()
    add_executable(fiber_tests
        tests/SanityTest.cpp
        tests/GeneratorTest.cpp
        tests/ParserTest.cpp
    )
    target_link_libraries(fiber_tests PRIVATE fiber_lib GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(fiber_tests)
endif()
