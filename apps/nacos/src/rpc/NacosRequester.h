#ifndef FIBER_NACOS_RPC_NACOS_REQUESTER_H
#define FIBER_NACOS_RPC_NACOS_REQUESTER_H

#include <chrono>
#include <cstddef>
#include <expected>

#include <async/Task.h>
#include <grpc/GrpcClient.h>

#include "NacosPayloadCodec.h"

namespace fiber::nacos::detail {

class NacosRequester {
public:
    NacosRequester(grpc::GrpcClient &client, std::size_t max_payload_bytes,
                   std::chrono::milliseconds request_timeout) noexcept :
        client_(&client), max_payload_bytes_(max_payload_bytes), request_timeout_(request_timeout) {}

    template<typename Request, typename Response>
    [[nodiscard]] async::Task<std::expected<void, NacosRpcError>>
    request(const Request &request, const NacosPayloadMetadata &metadata, mem::BufPool &pool,
            Response &response) noexcept {
        auto payload = encode_payload(request, metadata, max_payload_bytes_);
        if (!payload) {
            co_return std::unexpected(std::move(payload.error()));
        }
        auto response_payload = co_await request_payload(*payload, pool);
        if (!response_payload) {
            co_return std::unexpected(std::move(response_payload.error()));
        }
        co_return decode_payload(*response_payload, max_payload_bytes_, pool, response);
    }

private:
    [[nodiscard]] async::Task<std::expected<proto::Payload, NacosRpcError>>
    request_payload(const proto::Payload &request, mem::BufPool &pool) noexcept;

    grpc::GrpcClient *client_ = nullptr;
    std::size_t max_payload_bytes_ = 0;
    std::chrono::milliseconds request_timeout_{};
};

} // namespace fiber::nacos::detail

#endif // FIBER_NACOS_RPC_NACOS_REQUESTER_H
