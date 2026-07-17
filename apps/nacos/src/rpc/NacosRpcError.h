#ifndef FIBER_NACOS_RPC_NACOS_RPC_ERROR_H
#define FIBER_NACOS_RPC_NACOS_RPC_ERROR_H

#include <cstdint>
#include <string>

#include <common/IoError.h>

namespace fiber::nacos::detail {

enum class NacosRpcErrorCode : std::uint8_t {
    AuthenticationUnavailable,
    Transport,
    GrpcStatus,
    Protocol,
    Server,
    QueueFull,
    Shutdown,
};

struct NacosRpcError {
    NacosRpcErrorCode code = NacosRpcErrorCode::Protocol;
    common::IoErr io_error = common::IoErr::None;
    int grpc_status = 0;
    std::int32_t result_code = 0;
    std::int32_t error_code = 0;
    std::string message;
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_RPC_ERROR_H
