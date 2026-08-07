#ifndef FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
#define FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H

#include <cstddef>
#include <cstdint>

#include "../common/IntrusiveList.h"
#include "Http2Outbound.h"

namespace fiber::http {

class Http2Connection;

class Http2OutboundHook {
public:
    [[nodiscard]] bool idle() const noexcept { return state_ == State::Idle; }

private:
    using SendDoneCallback = void (*)(Http2OutboundHook &hook, common::IoErr state) noexcept;

    enum class State : std::uint8_t {
        Idle = 0,
        Queued,
        InFlight,
    };

    common::IntrusiveListHook queue_hook_{};
    mem::IoBufChain encoded_{};
    void *ctx_ = nullptr;
    SendDoneCallback send_done_cb_ = nullptr;
    std::size_t inflight_wire_bytes_ = 0;
    std::uint32_t window_consumed_ = 0;
    common::IoErr completion_result_ = common::IoErr::None;
    bool operation_final_batch_ = false;
    State state_ = State::Idle;

    friend class Http2Connection;
    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
