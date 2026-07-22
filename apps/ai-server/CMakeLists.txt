add_library(ai_server_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/AiServer.cpp")
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
