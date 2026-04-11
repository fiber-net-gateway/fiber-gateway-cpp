set(FIBER_SIMPLE_EXAMPLES
    dns_dig
    git_http_repo_server
    http1_echo
    https_echo
    tcp_echo
    udp_echo
)

foreach(FIBER_EXAMPLE_NAME IN LISTS FIBER_SIMPLE_EXAMPLES)
    fiber_add_example(${FIBER_EXAMPLE_NAME}
        SOURCES "${CMAKE_CURRENT_LIST_DIR}/${FIBER_EXAMPLE_NAME}.cpp")
endforeach()

if (TARGET lsquic::lsquic)
    fiber_add_example(http3_demo_lsquic
        SOURCES "${CMAKE_CURRENT_LIST_DIR}/http3_demo_lsquic.cpp"
        LIBRARIES
            lsquic::lsquic
            boringssl::ssl
            boringssl::crypto
            ${FIBER_ZLIB_TARGET})
    if (NOT FIBER_USE_LIBCXX)
        target_link_libraries(http3_demo_lsquic PRIVATE stdc++exp)
    endif()
    if (DEFINED lsquic_SOURCE_DIR)
        target_include_directories(http3_demo_lsquic PRIVATE
            ${lsquic_SOURCE_DIR}/include
            ${lsquic_SOURCE_DIR}/src/liblsquic)
    else()
        target_include_directories(http3_demo_lsquic PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/../temp/_deps/lsquic-src/include
            ${CMAKE_CURRENT_LIST_DIR}/../temp/_deps/lsquic-src/src/liblsquic)
    endif()
else()
    message(STATUS "Skipping example http3_demo_lsquic because lsquic is unavailable")
endif()
