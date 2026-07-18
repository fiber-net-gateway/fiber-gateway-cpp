#ifndef FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
#define FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H

#include "../common/IntrusiveList.h"
#include "Http2Outbound.h"

namespace fiber::http {

class Http2OutboundScheduler;
class Http2Connection;

class Http2OutboundHook {
private:
    common::IntrusiveListHook queue_hook_{};
    Http2OutboundNextKind next_kind_ = Http2OutboundNextKind::None;
    std::uint8_t queue_state_ = 0;
    bool closed_ = false;

    friend class Http2Connection;
    friend class Http2OutboundScheduler;
    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
