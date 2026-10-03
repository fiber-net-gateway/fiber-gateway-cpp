# libFuzzer harnesses (FIBER_BUILD_FUZZERS=ON; see README.md). The top-level
# option already instruments every fiber target with ASan/UBSan and
# fuzzer-no-link coverage; each harness adds the libFuzzer runtime (main).

set(FIBER_FUZZERS
    tls_handshake_codec_fuzzer
    tls_server_engine_fuzzer
    tls_client_engine_fuzzer
    tls_connection_fuzzer
    quic_codec_fuzzer
    quic_reassembly_fuzzer
    quic_connection_fuzzer
    quic_endpoint_fuzzer
    quic_client_fuzzer
    quic_network_fuzzer
)

foreach(fuzzer IN LISTS FIBER_FUZZERS)
    add_executable(${fuzzer} "${CMAKE_CURRENT_LIST_DIR}/${fuzzer}.cpp")
    # The handshake engines are src-side headers (src/tls/handshake).
    target_include_directories(${fuzzer} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${fuzzer} PRIVATE fiber_lib)
    target_link_options(${fuzzer} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${fuzzer} PROPERTIES FOLDER "fuzz")

    # Replay as a regression test: every committed seed and every fixed-crash
    # reproducer runs once (regressions/ survives seed regeneration).
    set(replay_dirs)
    foreach(dir IN ITEMS corpus regressions)
        if (EXISTS "${CMAKE_CURRENT_LIST_DIR}/${dir}/${fuzzer}")
            list(APPEND replay_dirs "${CMAKE_CURRENT_LIST_DIR}/${dir}/${fuzzer}")
        endif()
    endforeach()
    if (replay_dirs)
        add_test(NAME ${fuzzer}_replay COMMAND ${fuzzer} -runs=0 ${replay_dirs})
    endif()
endforeach()

# QUIC seed generator: builds the QUIC harness seeds with the in-tree
# encoders and lifts a real ClientHello from the in-tree QUIC client.
add_executable(quic_fuzz_seedgen "${CMAKE_CURRENT_LIST_DIR}/quic_fuzz_seedgen.cpp")
target_include_directories(quic_fuzz_seedgen PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(quic_fuzz_seedgen PRIVATE fiber_lib)
set_target_properties(quic_fuzz_seedgen PROPERTIES FOLDER "fuzz")

# Seed generator: drives the in-tree engines against BoringSSL peers and
# writes real handshake streams into corpus/ (see README.md). Not a fuzzer —
# it has its own main and links the ssl library as the peer.
add_executable(tls_fuzz_seedgen "${CMAKE_CURRENT_LIST_DIR}/tls_fuzz_seedgen.cpp")
target_include_directories(tls_fuzz_seedgen PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(tls_fuzz_seedgen PRIVATE fiber_lib boringssl::ssl)
set_target_properties(tls_fuzz_seedgen PROPERTIES FOLDER "fuzz")
