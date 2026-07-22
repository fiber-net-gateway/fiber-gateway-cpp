set(FIBER_SIMPLE_EXAMPLES
    dns_dig
    git_http_repo_server
    http_benchmark_backend
    http3_benchmark_client
    http3_benchmark_server
    http1_echo
    https_echo
    tcp_echo
    udp_echo
)

foreach(FIBER_EXAMPLE_NAME IN LISTS FIBER_SIMPLE_EXAMPLES)
    fiber_add_example(${FIBER_EXAMPLE_NAME}
        SOURCES "${CMAKE_CURRENT_LIST_DIR}/${FIBER_EXAMPLE_NAME}.cpp")
endforeach()

# nacos_demo links the nacos application library (fiber::nacos), which is defined under
# apps/. The top-level CMake adds apps/ before example/ so the target is available here.
if(TARGET fiber::nacos)
    fiber_add_example(nacos_demo
        SOURCES "${CMAKE_CURRENT_LIST_DIR}/nacos_demo.cpp"
        LIBRARIES fiber::nacos)
endif()
