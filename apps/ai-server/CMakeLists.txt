add_library(ai_server_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServerConfig.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServerRuntime.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/LlmConfigCodec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/LlmConfigManager.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/LlmConfigSnapshot.cpp")
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
        "${CMAKE_CURRENT_LIST_DIR}/tests/AiServerConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmConfigCodecTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LlmConfigManagerTest.cpp")
    target_link_libraries(fiber_ai_server_tests PRIVATE ai_server_core GTest::gtest_main)
    target_compile_definitions(fiber_ai_server_tests PRIVATE
        FIBER_AI_SERVER_TEST_ENV_PATH="${CMAKE_CURRENT_LIST_DIR}/ai-server.env.example")
    set_target_properties(fiber_ai_server_tests PROPERTIES FOLDER "apps/ai-server")
    fiber_link_runtime_allocator(fiber_ai_server_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_ai_server_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_ai_server_tests)
endif()
