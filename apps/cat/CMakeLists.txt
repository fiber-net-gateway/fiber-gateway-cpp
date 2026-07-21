add_library(fiber_cat STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/CatInternal.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Event.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Message.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Metric.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/Transaction.cpp")
add_library(fiber::cat ALIAS fiber_cat)

target_include_directories(fiber_cat
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(fiber_cat PUBLIC fiber_lib)
set_target_properties(fiber_cat PROPERTIES FOLDER "apps/cat")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET fiber_cat PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(fiber_cat_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/MessageTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/MetricTest.cpp")
    target_link_libraries(fiber_cat_tests PRIVATE fiber::cat GTest::gtest_main)
    set_target_properties(fiber_cat_tests PROPERTIES FOLDER "apps/cat")
    fiber_link_runtime_allocator(fiber_cat_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_cat_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_cat_tests)
endif()
