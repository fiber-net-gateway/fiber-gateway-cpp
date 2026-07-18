#ifndef FIBER_HTTP_HTTP2_OUTBOUND_H
#define FIBER_HTTP_HTTP2_OUTBOUND_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"

namespace fiber::http {

class Http2Stream;

enum class Http2OutboundNextKind : std::uint8_t {
    None = 0,
    Headers,
    Data,
};

struct Http2OutboundEncodeRequest {
    std::uint32_t max_frame_size = 0;
    std::uint32_t conn_window_budget = 0;
    std::uint32_t stream_window_budget = 0;
};

struct Http2OutboundEncodeResult {
    enum class Status : std::uint8_t {
        Encoded = 0,
        NoWork,
        BlockedConnWindow,
        Closed,
    };

    Status status = Status::NoWork;
    Http2OutboundNextKind next_kind = Http2OutboundNextKind::None;
    std::uint32_t flow_controlled_bytes = 0;
};

class Http2OutboundEncodeTarget {
public:
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t total_bytes() const noexcept;

    [[nodiscard]] common::IoErr append_copy(const void *src, std::size_t bytes) noexcept;
    [[nodiscard]] common::IoErr append_buffer(mem::IoBuf &&buf) noexcept;
    [[nodiscard]] common::IoErr append_chain(mem::IoBufChain &&chain) noexcept;

private:
    void reset(mem::IoBufNodePool &node_pool) noexcept;
    [[nodiscard]] mem::IoBufChain take_chain() noexcept { return std::move(chain_); }

    mem::IoBufChain chain_{};

    friend class Http2OutboundScheduler;
};

class Http2OutboundOperation {
public:
    virtual common::IoErr encode_outbound_batch(Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                                Http2OutboundEncodeTarget &target,
                                                Http2OutboundEncodeResult &result) noexcept = 0;
    virtual void on_outbound_abort(common::IoErr reason) noexcept = 0;
    virtual void on_stream_send_window_available() noexcept {}

protected:
    virtual ~Http2OutboundOperation() = default;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_H
