#ifndef FIBER_HTTP_HTTP2_OUTBOUND_H
#define FIBER_HTTP_HTTP2_OUTBOUND_H

#include <cstddef>
#include <cstdint>
#include <utility>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
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
    bool operation_final_batch = false;
};

// Appends encoded frames straight onto a connection's in-flight chain. Small
// writes go into the chain's tail node when it alone owns its storage and has
// room, so consecutive frames share one buffer instead of allocating one each.
// Nothing appended reaches the transport before the encode returns, and a
// failed encode takes its bytes back with rollback().
class Http2OutboundEncodeTarget : public common::NonCopyable, public common::NonMovable {
public:
    explicit Http2OutboundEncodeTarget(mem::IoBufChain &chain) noexcept;
    ~Http2OutboundEncodeTarget();

    [[nodiscard]] bool empty() const noexcept { return total_bytes() == 0; }
    // Bytes appended since construction.
    [[nodiscard]] std::size_t total_bytes() const noexcept;

    // Contiguous room for at least `min_bytes` at the end of the chain: the
    // tail node's free space when that node alone owns its storage, else a
    // fresh buffer of max(min_bytes, capacity_hint) that joins the chain on
    // its first commit. The room stays valid until the next acquire or append.
    [[nodiscard]] common::IoErr acquire(std::size_t min_bytes, std::size_t capacity_hint, std::uint8_t *&dst,
                                        std::size_t &len) noexcept;
    // Makes the next `bytes` of the acquired room readable.
    void commit(std::size_t bytes) noexcept;

    [[nodiscard]] common::IoErr append_copy(const void *src, std::size_t bytes) noexcept;
    // Moves the chain's nodes in. Its completion marker belongs to the
    // payload's producer and is dropped.
    [[nodiscard]] common::IoErr append_chain(mem::IoBufChain &&chain) noexcept;

    // Takes back everything appended since construction.
    void rollback() noexcept;

private:
    void release_pending() noexcept;

    mem::IoBufChain *chain_;
    std::size_t base_bytes_;
    // A fresh buffer and its node, linked into the chain by the first commit:
    // an empty node must never sit in the chain.
    mem::IoBufNode *pending_ = nullptr;
};

struct Http2OutboundOperation {
    struct Ops {
        common::IoErr (*on_encode)(void *ctx, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                                   Http2OutboundEncodeTarget &target,
                                   Http2OutboundEncodeResult &result) noexcept = nullptr;
        void (*on_send_done)(void *ctx, const Http2OutboundSendResult &result) noexcept = nullptr;
        bool allow_partial_final_batch = false;
    };

    const Ops *ops = nullptr;
    void *ctx = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept { return ops != nullptr; }
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_H
