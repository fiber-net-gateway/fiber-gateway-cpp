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

class Http2OutboundOperation {
public:
    [[nodiscard]] virtual std::size_t pending_flow_controlled_bytes() const noexcept = 0;
    virtual common::IoErr encode_outbound_batch(Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                                Http2OutboundEncodeTarget &target,
                                                Http2OutboundEncodeResult &result) noexcept = 0;
    virtual void on_outbound_batch_sent(std::uint32_t flow_controlled_bytes, bool operation_final_batch) noexcept = 0;
    virtual void on_outbound_abort(common::IoErr reason) noexcept = 0;

protected:
    virtual ~Http2OutboundOperation() = default;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_H
