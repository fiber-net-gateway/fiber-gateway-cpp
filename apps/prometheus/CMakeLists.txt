add_library(fiber_prometheus STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/MetricValue.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/MetricsRegistry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/TextEncoder.cpp")
add_library(fiber::prometheus ALIAS fiber_prometheus)

target_include_directories(fiber_prometheus
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(fiber_prometheus PUBLIC fiber_lib)
set_target_properties(fiber_prometheus PROPERTIES FOLDER "apps/prometheus")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET fiber_prometheus PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_executable(prometheus_record_benchmark EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_LIST_DIR}/bench/RecordBenchmark.cpp")
target_link_libraries(prometheus_record_benchmark PRIVATE fiber::prometheus)
set_target_properties(prometheus_record_benchmark PROPERTIES FOLDER "apps/prometheus")
fiber_link_runtime_allocator(prometheus_record_benchmark)

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(fiber_prometheus_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/MetricValueTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/MetricsRegistryTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/MultiLoopSnapshotTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/TextEncoderTest.cpp")
    target_link_libraries(fiber_prometheus_tests PRIVATE fiber::prometheus GTest::gtest_main)
    target_include_directories(fiber_prometheus_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
    set_target_properties(fiber_prometheus_tests PROPERTIES FOLDER "apps/prometheus")
    fiber_link_runtime_allocator(fiber_prometheus_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_prometheus_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_prometheus_tests)
endif()
