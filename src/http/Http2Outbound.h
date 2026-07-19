#ifndef FIBER_HTTP_HTTP2_OUTBOUND_H
#define FIBER_HTTP_HTTP2_OUTBOUND_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::http {

class Http2Stream;

enum class Http2OutboundKind : std::uint8_t {
    None = 0,
    Headers,
    Data,
};

struct Http2OutboundEncodeRequest {
    std::uint32_t max_frame_size = 0;
    std::uint32_t payload_budget = 0;
};

struct Http2OutboundEncodeResult {
    std::uint32_t flow_controlled_bytes = 0;
    bool operation_final_batch = false;
};

struct Http2OutboundSendResult {
    common::IoErr error = common::IoErr::None;
    std::uint32_t flow_controlled_bytes = 0;
    bool operation_final_batch = false;
};

class Http2OutboundEncodeTarget {
public:
    explicit Http2OutboundEncodeTarget(mem::IoBufNodePool &node_pool) noexcept : chain_(node_pool) {}

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t total_bytes() const noexcept;
    [[nodiscard]] mem::IoBufChain take_chain() noexcept { return std::move(chain_); }

    [[nodiscard]] common::IoErr append_copy(const void *src, std::size_t bytes) noexcept;
    [[nodiscard]] common::IoErr append_buffer(mem::IoBuf &&buf) noexcept;
    [[nodiscard]] common::IoErr append_chain(mem::IoBufChain &&chain) noexcept;

private:
    mem::IoBufChain chain_{};
};

struct Http2OutboundOperation {
    struct Ops {
        common::IoErr (*on_encode)(void *ctx, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                   Http2OutboundEncodeTarget &target,
                                   Http2OutboundEncodeResult &result) noexcept = nullptr;
        void (*on_send_done)(void *ctx, const Http2OutboundSendResult &result) noexcept = nullptr;
    };

    const Ops *ops = nullptr;
    void *ctx = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return ops != nullptr; }
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_H
