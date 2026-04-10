add_library(lite_nginx_config STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/config/ConfigLoader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/Lexer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/Parser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/Semantic.cpp")
target_include_directories(lite_nginx_config PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
set_target_properties(lite_nginx_config PROPERTIES FOLDER "apps/lite_nginx")
if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET lite_nginx_config PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

add_library(lite_nginx_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/app/App.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/RuntimeBuilder.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/runtime/ServerLauncher.cpp")
target_include_directories(lite_nginx_core PUBLIC "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(lite_nginx_core PUBLIC fiber_lib lite_nginx_config)
target_compile_definitions(lite_nginx_core PRIVATE
    FIBER_LITE_NGINX_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
set_target_properties(lite_nginx_core PROPERTIES FOLDER "apps/lite_nginx")
if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET lite_nginx_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

fiber_add_app(lite_nginx
    SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/src/main.cpp"
    LIBRARIES
        lite_nginx_core)

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)

    add_executable(lite_nginx_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/LiteNginxConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/LiteNginxRuntimeTest.cpp")
    target_link_libraries(lite_nginx_tests PRIVATE lite_nginx_core GTest::gtest_main)
    target_include_directories(lite_nginx_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
    set_target_properties(lite_nginx_tests PROPERTIES FOLDER "apps/lite_nginx")
    fiber_link_runtime_allocator(lite_nginx_tests)
    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET lite_nginx_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    gtest_discover_tests(lite_nginx_tests)
endif()
