add_library(ai_server_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServerConfig.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServerLogging.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServerRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/audit/LlmAuditLog.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/auth/Bt1TokenVerifier.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/auth/LlmRequestAuthenticator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/LlmConfigCodec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/LlmConfigManager.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/LlmConfigSnapshot.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/discovery/LoadBalancer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/discovery/ServiceDiscovery.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/RateLimitHash.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/RateLimitHttpCodec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/RateLimitClusterMembership.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/RateLimitShardRing.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/TokenRateLimitCoordinator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/TokenRateLimitRemoteClient.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/TokenRateLimitService.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/limit/TokenRateLimiter.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/observability/AiServerCatRequest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/observability/AiServerMetrics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/observability/ProcessMetrics.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/ProviderConnectionManager.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/ProviderErrorClassifier.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/ExecutionPlan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/ProviderEndpoint.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/ProviderHttpClient.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/ProviderRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/provider/WorkerDnsService.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/protocol/LlmBodyExtractor.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/protocol/LlmBodyModifier.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/protocol/LlmError.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/protocol/SseParser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/protocol/TokenUsage.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/ModelAuthorization.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/routing/ProviderRouteKey.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/server/LlmRequestHandler.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/server/TokenRateLimitHttpHandler.cpp")
target_include_directories(ai_server_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(ai_server_core
    PUBLIC
        fiber_lib
        fiber::nacos
        fiber::cat
        fiber::prometheus)
set_target_properties(ai_server_core PROPERTIES FOLDER "apps/ai-server")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET ai_server_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

fiber_add_app(ai_server
    OUTPUT_NAME ai-server
    SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp"
    LIBRARIES
        ai_server_core)

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(fiber_ai_server_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/AiServerCatRequestTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AiServerConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AiServerLoggingTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/AiServerMetricsTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/Bt1AuthenticationTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmBodyTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmConfigCodecTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmConfigManagerTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LoadBalancerTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmProtocolTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmProxyIntegrationTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmRateLimitTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmRoutingTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/WorkerDnsServiceTest.cpp")
    target_link_libraries(fiber_ai_server_tests PRIVATE ai_server_core GTest::gtest_main)
    target_compile_definitions(fiber_ai_server_tests PRIVATE
        FIBER_AI_SERVER_TEST_ENV_PATH="${CMAKE_CURRENT_LIST_DIR}/ai-server.env.example"
        FIBER_AI_SERVER_TEST_LOG_CONFIG_PATH="${CMAKE_CURRENT_LIST_DIR}/ai-server.logging.json.example")
    set_target_properties(fiber_ai_server_tests PROPERTIES FOLDER "apps/ai-server")
    fiber_link_runtime_allocator(fiber_ai_server_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_ai_server_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_ai_server_tests)
endif()
