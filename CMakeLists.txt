cmake_minimum_required(VERSION 4.1)

include(cmake/SelectToolchain.cmake)

project(fiber)

set(FIBER_DEPS_DIR "${CMAKE_CURRENT_LIST_DIR}/temp/_deps" CACHE PATH "Shared deps download directory")
set(FETCHCONTENT_BASE_DIR "${FIBER_DEPS_DIR}" CACHE PATH "FetchContent base directory")
option(FIBER_FETCH_DEPS "Allow CMake to download third-party dependencies" ON)
option(FIBER_USE_JEMALLOC "Link final executables against jemalloc" OFF)
option(FIBER_ENABLE_HTTP3 "Build in-tree HTTP/3 support" ON)
option(FIBER_ENABLE_UDP_GSO "Compile Linux UDP GSO support when available" ON)
option(FIBER_FORCE_TIMERFD_POLLER "Force the Linux Poller to use timerfd timeouts" OFF)
option(FIBER_ENABLE_BENCHMARK_TRACE "Enable benchmark-only hot-path trace counters" OFF)
option(FIBER_BUILD_APPS "Build multi-file applications under apps/" ON)
option(FIBER_BUILD_NACOS "Build the reusable fiber::nacos component" ${FIBER_BUILD_APPS})
option(FIBER_BUILD_CAT "Build the reusable fiber::cat component" ${FIBER_BUILD_APPS})
option(FIBER_BUILD_PROMETHEUS "Build the reusable fiber::prometheus component" ${FIBER_BUILD_APPS})
option(FIBER_BUILD_CAT_DEMO "Build the CAT client demo" OFF)
option(FIBER_BUILD_PROMETHEUS_BENCHMARK "Build the Prometheus record-path benchmark" OFF)
option(FIBER_ENABLE_SCRIPT_JIT "Build the LLVM 22 script JIT backend" OFF)
option(FIBER_BUILD_SCRIPT_BENCHMARK "Build the interpreter/LLVM JIT script benchmark" OFF)

include(cmake/Deps.cmake)
include(cmake/CompilerChecks.cmake)
include(cmake/FiberProtobuf.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
fiber_validate_cxx_toolchain()
fiber_detect_epoll_pwait2_syscall()
fiber_detect_udp_gso()

set(FIBER_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(cmake/FiberPlatformConfig.h.in
    "${FIBER_GENERATED_INCLUDE_DIR}/fiber/FiberPlatformConfig.h")

file(GLOB_RECURSE CPP_SRC_LIST CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)

add_library(fiber_lib STATIC ${CPP_SRC_LIST})
add_library(fiber_allocator INTERFACE)
add_library(fiber_stdlib INTERFACE)
target_include_directories(fiber_lib
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${FIBER_GENERATED_INCLUDE_DIR})
target_link_libraries(fiber_lib PUBLIC boringssl::ssl boringssl::crypto)
target_compile_definitions(fiber_lib
    PUBLIC
        FIBER_ENABLE_BENCHMARK_TRACE=$<BOOL:${FIBER_ENABLE_BENCHMARK_TRACE}>
        FIBER_ENABLE_SCRIPT_JIT=$<BOOL:${FIBER_ENABLE_SCRIPT_JIT}>
    PRIVATE
        _GNU_SOURCE
        FIBER_HAVE_UDP_SEGMENT=$<BOOL:${FIBER_HAVE_UDP_SEGMENT}>)

if (FIBER_ENABLE_SCRIPT_JIT)
    find_package(LLVM REQUIRED CONFIG)
    if (NOT LLVM_VERSION_MAJOR EQUAL 22)
        message(FATAL_ERROR "The script JIT requires LLVM 22; found ${LLVM_PACKAGE_VERSION}")
    endif()
    if (NOT TARGET LLVM)
        message(FATAL_ERROR "LLVM 22 package does not provide the LLVM imported target")
    endif()
    target_include_directories(fiber_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_link_libraries(fiber_lib PRIVATE LLVM)
endif()
set(FIBER_CLANG_CONSTEXPR_STEPS "16777216" CACHE STRING
    "Clang constexpr evaluator step limit for compile-time generated tables")
target_compile_options(fiber_lib PRIVATE
    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fconstexpr-steps=${FIBER_CLANG_CONSTEXPR_STEPS}>")
if (FIBER_USE_LIBCXX)
    if (FIBER_STDLIB_LINK_OPTIONS)
        target_link_options(fiber_stdlib INTERFACE ${FIBER_STDLIB_LINK_OPTIONS})
    endif()
    if (FIBER_STDLIB_LINK_LIBRARIES)
        target_link_libraries(fiber_stdlib INTERFACE ${FIBER_STDLIB_LINK_LIBRARIES})
    endif()
    target_link_libraries(fiber_lib PUBLIC fiber_stdlib)
endif()

if (FIBER_USE_JEMALLOC)
    set(FIBER_JEMALLOC_TARGET "")
    find_package(jemalloc CONFIG QUIET)
    if (TARGET jemalloc::jemalloc)
        set(FIBER_JEMALLOC_TARGET jemalloc::jemalloc)
    elseif (TARGET jemalloc)
        set(FIBER_JEMALLOC_TARGET jemalloc)
    endif()

    if (NOT FIBER_JEMALLOC_TARGET)
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(JEMALLOC QUIET IMPORTED_TARGET jemalloc)
            if (TARGET PkgConfig::JEMALLOC)
                set(FIBER_JEMALLOC_TARGET PkgConfig::JEMALLOC)
            endif()
        endif()
    endif()

    if (NOT FIBER_JEMALLOC_TARGET)
        find_library(FIBER_JEMALLOC_LIBRARY NAMES jemalloc)
        if (FIBER_JEMALLOC_LIBRARY)
            add_library(fiber_jemalloc UNKNOWN IMPORTED)
            set_target_properties(fiber_jemalloc PROPERTIES
                IMPORTED_LOCATION "${FIBER_JEMALLOC_LIBRARY}")
            set(FIBER_JEMALLOC_TARGET fiber_jemalloc)
        endif()
    endif()

    if (NOT FIBER_JEMALLOC_TARGET AND FIBER_FETCH_DEPS)
        fiber_prepare_jemalloc_target()
        if (TARGET fiber_jemalloc)
            set(FIBER_JEMALLOC_TARGET fiber_jemalloc)
        endif()
    endif()

    if (NOT FIBER_JEMALLOC_TARGET)
        message(FATAL_ERROR
            "FIBER_USE_JEMALLOC=ON but jemalloc was not found. "
            "Install jemalloc or enable FIBER_FETCH_DEPS=ON to download it automatically.")
    endif()

    target_link_libraries(fiber_allocator INTERFACE ${FIBER_JEMALLOC_TARGET})
    if (UNIX AND NOT APPLE)
        target_link_options(fiber_allocator INTERFACE -Wl,--no-as-needed)
    endif()
endif()

function(fiber_link_runtime_allocator target_name)
    target_link_libraries(${target_name} PRIVATE fiber_allocator)
    if (TARGET fiber_jemalloc_ep)
        add_dependencies(${target_name} fiber_jemalloc_ep)
    endif()
endfunction()

include(cmake/FiberExecutable.cmake)

option(FIBER_ENABLE_LTO "Enable LTO/IPO for fiber targets" ON)
option(FIBER_ALLOW_GCC_LTO "Allow LTO/IPO when building with GCC" OFF)
set(FIBER_IPO_SUPPORTED FALSE)
if (FIBER_ENABLE_LTO)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT FIBER_ALLOW_GCC_LTO)
        message(STATUS
            "Disabling IPO/LTO for GCC builds by default to avoid unstable "
            "lto-wrapper failures when linking fiber_lib. "
            "Set FIBER_ALLOW_GCC_LTO=ON to override.")
    else()
        include(CheckIPOSupported)
        check_ipo_supported(RESULT FIBER_IPO_SUPPORTED OUTPUT FIBER_IPO_ERROR)
        if (FIBER_IPO_SUPPORTED)
            set_property(TARGET fiber_lib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(STATUS "IPO/LTO not supported: ${FIBER_IPO_ERROR}")
        endif()
    endif()
endif()

if (FIBER_BUILD_SCRIPT_BENCHMARK)
    if (NOT FIBER_ENABLE_SCRIPT_JIT)
        message(FATAL_ERROR
            "FIBER_BUILD_SCRIPT_BENCHMARK requires FIBER_ENABLE_SCRIPT_JIT=ON")
    endif()
    add_executable(script_jit_benchmark
        "${CMAKE_CURRENT_LIST_DIR}/benchmark/script/ScriptJitBenchmark.cpp")
    target_link_libraries(script_jit_benchmark PRIVATE fiber_lib)
    target_compile_definitions(script_jit_benchmark PRIVATE
        FIBER_SCRIPT_BENCHMARK_BUILD_TYPE="$<CONFIG>")
    set_target_properties(script_jit_benchmark PROPERTIES FOLDER "benchmarks")
    fiber_link_runtime_allocator(script_jit_benchmark)
    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET script_jit_benchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

option(FIBER_BUILD_EXAMPLES "Build simple demo examples" ON)

option(FIBER_BUILD_TESTS "Build tests" ON)
if(FIBER_BUILD_TESTS)
    find_package(GTest QUIET)
    if (NOT GTest_FOUND)
        if (FIBER_FETCH_DEPS)
            include(FetchContent)
            set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
            fiber_use_cached_content(googletest)
            FetchContent_Declare(
                googletest
                URL "${FIBER_GOOGLETEST_URL}"
                URL_HASH "SHA256=${FIBER_GOOGLETEST_SHA256}"
            )
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(googletest)
        else()
            message(STATUS "GoogleTest not found. Set FIBER_FETCH_DEPS=ON or install GTest to build tests.")
        endif()
    endif()
    if (GTest_FOUND OR TARGET GTest::gtest_main)
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            if (TARGET gtest)
                target_compile_options(gtest PRIVATE -Wno-character-conversion)
            endif()
            if (TARGET gtest_main)
                target_compile_options(gtest_main PRIVATE -Wno-character-conversion)
            endif()
        endif()

        enable_testing()
        file(GLOB FIBER_TEST_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_LIST_DIR}/tests/*Test.cpp")
        list(SORT FIBER_TEST_SOURCES)
        if (FIBER_TEST_SOURCES)
            add_executable(fiber_tests ${FIBER_TEST_SOURCES})
            target_link_libraries(fiber_tests PRIVATE fiber_lib GTest::gtest_main)
            fiber_link_runtime_allocator(fiber_tests)
            if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
                set_property(TARGET fiber_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            endif()
            include(GoogleTest)
            gtest_discover_tests(fiber_tests)
        else()
            message(STATUS "No tests found under tests/*. Tests may be archived under temp/.")
        endif()
    endif()
endif()

if (FIBER_BUILD_NACOS)
    add_subdirectory(apps/nacos "${CMAKE_BINARY_DIR}/components/nacos")
endif()
if (FIBER_BUILD_CAT)
    add_subdirectory(apps/cat "${CMAKE_BINARY_DIR}/components/cat")
endif()
if (FIBER_BUILD_PROMETHEUS)
    add_subdirectory(apps/prometheus "${CMAKE_BINARY_DIR}/components/prometheus")
endif()

if (FIBER_BUILD_APPS AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/apps/CMakeLists.txt")
    add_subdirectory(apps)
endif()

# Examples are added after components and apps so that examples such as
# nacos_demo can resolve their reusable library targets.
if (FIBER_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()
