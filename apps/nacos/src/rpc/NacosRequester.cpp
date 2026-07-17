#include "NacosRequester.h"

namespace fiber::nacos::detail {
namespace {

NacosRpcError transport_error(common::IoErr error) {
    return NacosRpcError{
            .code = NacosRpcErrorCode::Transport,
            .io_error = error,
    };
}

} // namespace

async::Task<std::expected<proto::Payload, NacosRpcError>> NacosRequester::request_payload(const proto::Payload &request,
                                                                                          mem::BufPool &pool) noexcept {
    grpc::GrpcStream stream = client_->open_stream("Request", "request", pool,
                                                   {
                                                           .deadline = request_timeout_,
                                                           .max_inbound_message_bytes = max_payload_bytes_,
                                                   });
    auto result = co_await stream.open();
    if (!result) {
        co_return std::unexpected(transport_error(result.error()));
    }
    result = co_await stream.write(request);
    if (!result) {
        co_return std::unexpected(transport_error(result.error()));
    }
    result = co_await stream.writes_done();
    if (!result) {
        co_return std::unexpected(transport_error(result.error()));
    }

    proto::Payload response;
    auto read_result = co_await stream.read(response);
    if (!read_result) {
        co_return std::unexpected(transport_error(read_result.error()));
    }
    if (*read_result != grpc::GrpcReadOutcome::Message) {
        co_return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "Nacos unary RPC returned no response",
        });
    }
    auto extra = co_await stream.read(response);
    if (!extra) {
        co_return std::unexpected(transport_error(extra.error()));
    }
    if (*extra == grpc::GrpcReadOutcome::Message) {
        co_return std::unexpected(NacosRpcError{
                .code = NacosRpcErrorCode::Protocol,
                .io_error = common::IoErr::Invalid,
                .message = "Nacos unary RPC returned multiple responses",
        });
    }

    auto status = co_await stream.finish();
    if (!status) {
        co_return std::unexpected(transport_error(status.error()));
    }
    if (!status->ok()) {
        NacosRpcError error{
                .code = NacosRpcErrorCode::GrpcStatus,
                .grpc_status = status->code,
        };
        error.message.assign(status->message.substr(0, 512));
        co_return std::unexpected(std::move(error));
    }
    co_return response;
}

} // namespace fiber::nacos::detail
