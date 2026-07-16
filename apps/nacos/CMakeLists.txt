add_library(fiber_nacos STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/NacosClient.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/NacosClientConfig.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/auth/AuthHttpOperation.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/auth/NacosAuthenticator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/detail/NacosClientImpl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/AuthTokenResponseJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/ConfigQueryRequestJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/NotifySubscriberResponseJson.cpp")
add_library(fiber::nacos ALIAS fiber_nacos)

target_include_directories(fiber_nacos
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(fiber_nacos PUBLIC fiber_lib)
set_target_properties(fiber_nacos PROPERTIES FOLDER "apps/nacos")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET fiber_nacos PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(fiber_nacos_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/DtoJsonTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/NacosClientConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/NacosClientTest.cpp")
    target_link_libraries(fiber_nacos_tests PRIVATE fiber::nacos GTest::gtest_main)
    set_target_properties(fiber_nacos_tests PROPERTIES FOLDER "apps/nacos")
    fiber_link_runtime_allocator(fiber_nacos_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_nacos_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_nacos_tests)
endif()
