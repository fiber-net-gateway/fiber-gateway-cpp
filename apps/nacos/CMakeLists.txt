fiber_proto_library(nacos_payload nacos_grpc_payload.proto)

add_library(fiber_nacos STATIC
    "${CMAKE_CURRENT_LIST_DIR}/src/NacosClient.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/NacosClientConfig.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/NamingService.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/detail/NacosClientImpl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/config/ConfigServiceImpl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/naming/NamingServiceImpl.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/ConfigJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/ConfigQueryRequestJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/InternalJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/JsonDecode.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/NamingJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/dto/NotifySubscriberResponseJson.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rpc/grpc/GrpcClient.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rpc/grpc/GrpcFraming.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rpc/grpc/GrpcStream.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rpc/grpc/ProtoCodec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rpc/NacosPayloadCodec.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/src/rpc/NacosRpc.cpp")
add_library(fiber::nacos ALIAS fiber_nacos)

target_include_directories(fiber_nacos
    PUBLIC
        "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(fiber_nacos
    PUBLIC fiber_lib
    PRIVATE fiber_proto_nacos_payload)
set_target_properties(fiber_nacos PROPERTIES FOLDER "apps/nacos")

if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
    set_property(TARGET fiber_nacos PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (FIBER_BUILD_TESTS AND (GTest_FOUND OR TARGET GTest::gtest_main))
    include(GoogleTest)
    fiber_proto_library(nacos_grpc_test helloworld.proto)

    add_executable(fiber_nacos_tests
        "${CMAKE_CURRENT_LIST_DIR}/tests/DtoJsonTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ConfigServiceTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/NacosClientConfigTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/NacosClientTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/NamingServiceTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/NacosRpcTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/PayloadTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/RpcDtoJsonTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/ServiceDiscoveryTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/SubscriptionPoolTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/rpc/GrpcClientTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/rpc/GrpcFramingTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/rpc/GrpcProtoCodecTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/rpc/GrpcStatusTest.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/tests/rpc/GrpcStreamTest.cpp")
    target_include_directories(fiber_nacos_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
    target_link_libraries(fiber_nacos_tests PRIVATE
        fiber::nacos
        fiber_proto_nacos_payload
        fiber_proto_nacos_grpc_test
        GTest::gtest_main)
    set_target_properties(fiber_nacos_tests PROPERTIES FOLDER "apps/nacos")
    fiber_link_runtime_allocator(fiber_nacos_tests)

    if (FIBER_ENABLE_LTO AND FIBER_IPO_SUPPORTED)
        set_property(TARGET fiber_nacos_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    gtest_discover_tests(fiber_nacos_tests)
endif()
