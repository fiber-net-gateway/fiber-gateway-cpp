#ifndef FIBER_GRPC_GRPC_STATUS_H
#define FIBER_GRPC_GRPC_STATUS_H

#include <string>
#include <string_view>

namespace fiber::grpc {

// gRPC call outcome. `code` is the grpc-status trailer value (0 == OK);
// `message` is grpc-message (raw, not percent-decoded). Transport/protocol
// failures are reported via IoErr on the IoResult instead.
struct GrpcStatus {
    int code = 0;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return code == 0; }
};

// Parse the grpc-status header/trailer value (ascii decimal). Returns -1 on
// overflow or non-numeric input; an empty input yields 0.
int parse_grpc_status(std::string_view s) noexcept;

} // namespace fiber::grpc

#endif // FIBER_GRPC_GRPC_STATUS_H
