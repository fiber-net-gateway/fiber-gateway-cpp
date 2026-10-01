#ifndef FIBER_HTTP_DETAIL_HTTP2_DATA_SEND_OPS_H
#define FIBER_HTTP_DETAIL_HTTP2_DATA_SEND_OPS_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/Http2Outbound.h>

namespace fiber::http::detail {

// DATA send operations for Http2SendAwaiter, shared by both stream roles.

// Sends the whole chain, in as many batches as flow control needs.
struct Http2DataSendAllOp {
    inline static constexpr Http2OutboundKind kOutboundKind = Http2OutboundKind::Data;

    explicit Http2DataSendAllOp(mem::IoBufChain &&chunk) noexcept : chunk_(std::move(chunk)), end_(chunk_.complete()) {}

    [[nodiscard]] std::size_t flow_controlled_bytes() const noexcept { return chunk_.readable_bytes(); }
    [[nodiscard]] bool end_stream() const noexcept { return end_; }

    common::IoErr on_encode(const Http2OutboundEncodeRequest &req, Http2OutboundEncodeTarget &target,
                            Http2OutboundEncodeResult &result) noexcept;

    mem::IoBufChain chunk_;
    // Read once up front: encoding the last bytes moves the chain's
    // completion marker along with them.
    bool end_ = false;
};

// Sends what one batch's budget allows from a borrowed chain or buffer.
struct Http2DataSendSomeOp {
    inline static constexpr Http2OutboundKind kOutboundKind = Http2OutboundKind::Data;
    inline static constexpr bool kAllowsPartialFinalBatch = true;

    explicit Http2DataSendSomeOp(mem::IoBufChain &chunk) noexcept :
        chunk_(&chunk), total_bytes_(chunk.readable_bytes()), end_(chunk.complete()) {}

    Http2DataSendSomeOp(const std::uint8_t *buf, std::size_t len, bool end) noexcept :
        buf_(buf), total_bytes_(len), end_(end) {}

    [[nodiscard]] std::size_t flow_controlled_bytes() const noexcept { return total_bytes_; }
    [[nodiscard]] bool end_stream() const noexcept { return end_; }

    common::IoErr on_encode(const Http2OutboundEncodeRequest &req, Http2OutboundEncodeTarget &target,
                            Http2OutboundEncodeResult &result) noexcept;

    mem::IoBufChain *chunk_ = nullptr;
    const std::uint8_t *buf_ = nullptr;
    std::size_t total_bytes_ = 0;
    bool end_ = false;
};

} // namespace fiber::http::detail

#endif // FIBER_HTTP_DETAIL_HTTP2_DATA_SEND_OPS_H
