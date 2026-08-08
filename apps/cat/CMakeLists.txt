add_library(fiber_cat STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/CatAggregation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatClient.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatClientConfig.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatClientCore.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatEncoder.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatInternal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatMessageId.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatRouter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatSystemMessage.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/CatSystemStats.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Event.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/MessageTrace.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Metric.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Transaction.cpp")
add_library(fiber::cat ALIAS fiber_cat)

target_include_directories(fiber_cat
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(fiber_cat PUBLIC fiber_lib)
set_target_properties(fiber_cat PROPERTIES FOLDER "components/cat")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET fiber_cat PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(fiber_cat_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/AggregationTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/CatClientTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/EncoderTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/MessageIdTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/MessageTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/MetricTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/RouterTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/SystemStatsTest.cpp")
    target_include_directories(fiber_cat_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(fiber_cat_tests PRIVATE fiber::cat GTest::gtest_main)
    set_target_properties(fiber_cat_tests PROPERTIES FOLDER "components/cat")
    fiber_link_runtime_allocator(fiber_cat_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_cat_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_cat_tests)
endif()

if (FIBER_BUILD_CAT_DEMO)
    add_subdirectory(demo)
endif()
