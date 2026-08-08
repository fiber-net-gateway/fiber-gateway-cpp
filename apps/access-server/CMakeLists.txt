if (NOT TARGET fiber::nacos OR NOT TARGET fiber::cat OR NOT TARGET fiber::prometheus)
    message(STATUS "Skipping access-server because its Nacos, CAT, or Prometheus component is disabled")
    return()
endif()

add_library(access_server_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/config/AccessConfigCodec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/AccessResult.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/AccessRequestHandler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/ErrorResponder.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/ProxyExecutor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/ProxyResponsePlan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/ProxyUpstreamConnection.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/ResponseExecutor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/ResponsePlan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/execution/TemplateEvaluator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/observability/AccessRequestTelemetry.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/observability/AccessServerMetrics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/observability/AccessTraceState.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/AccessRouteSnapshot.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/Cidr.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/CompiledHeaderTemplates.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/CompiledTemplate.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/HostMatcher.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/ProjectRouteSnapshot.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/ProxyAddressSelector.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessConfigWatcher.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessScriptRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessDnsService.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessServer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessServerConfig.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessServerRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/GrayConfigWatcher.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/GrayMatchStore.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/AccessServiceDiscovery.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/RouteConfigStore.cpp")
target_include_directories(access_server_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(access_server_core PUBLIC fiber_lib fiber::nacos fiber::cat fiber::prometheus)
set_target_properties(access_server_core PROPERTIES FOLDER "apps/access-server")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET access_server_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

fiber_add_app(access_server
    OUTPUT_NAME access-server
    SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp"
    LIBRARIES
        access_server_core)

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(fiber_access_server_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessConfigCodecTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessConfigWatcherTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessRequestHandlerTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessServerTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessServerConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessTraceStateTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/CompiledHeaderTemplatesTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/CompiledTemplateTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/GrayConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AccessServiceDiscoveryTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ProductionScriptCorpusTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ProjectRouteSnapshotTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ProxyExecutorTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ProxyUpstreamConnectionTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ResponseExecutionTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/RouteConfigStoreTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/SmoothWeightedRoundRobinTest.cpp")
    target_link_libraries(fiber_access_server_tests PRIVATE access_server_core GTest::gtest_main)
    target_compile_definitions(fiber_access_server_tests PRIVATE
        FIBER_ACCESS_SERVER_TEST_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/tests/fixtures/java")
    set_target_properties(fiber_access_server_tests PROPERTIES FOLDER "apps/access-server")
    fiber_link_runtime_allocator(fiber_access_server_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_access_server_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_access_server_tests)
endif()
