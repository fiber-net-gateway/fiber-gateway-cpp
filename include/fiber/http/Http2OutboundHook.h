#ifndef FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
#define FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H

#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveList.h"
#include "../common/IoError.h"

namespace fiber::http {

class Http2Connection;

class Http2OutboundHook {
public:
    [[nodiscard]] bool idle() const noexcept { return state_ == State::Idle; }

private:
    using SendDoneCallback = void (*)(Http2OutboundHook &hook, common::IoErr state) noexcept;

    // The stream's whole outbound position. A stream sits in at most one
    // connection list, and the state names which one holds queue_hook_.
    enum class State : std::uint8_t {
        Idle = 0,
        // In the connection's ready queue. Nothing is encoded yet, so the send
        // can still be withdrawn.
        Ready,
        // DATA parked until the stream's own window opens; on no list.
        WaitStreamWindow,
        // DATA parked in the connection-window wait list.
        WaitConnWindow,
        // In the in-flight list: encoded into the connection's in-flight
        // chain, so it must reach the wire.
        InFlight,
    };

    common::IntrusiveListHook queue_hook_{};
    // Bound once by the owning stream.
    void *ctx_ = nullptr;
    SendDoneCallback send_done_cb_ = nullptr;
    // The connection's appended-byte position where this batch ends: the batch
    // is written once the connection's written-byte count reaches it.
    std::uint64_t inflight_end_ = 0;
    common::IoErr completion_result_ = common::IoErr::None;
    bool operation_final_batch_ = false;
    State state_ = State::Idle;

    friend class Http2Connection;
    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
