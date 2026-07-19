#ifndef FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
#define FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H

#include "../common/IntrusiveList.h"
#include "Http2Outbound.h"

namespace fiber::http {

class Http2OutboundScheduler;
class Http2Connection;


class Http2OutboundHook {

    using EncodeCallback = mem::IoBufChain (*)(Http2OutboundHook &hook) noexcept;
    using SendDoneCallback = void (*)(Http2OutboundHook &hook, common::IoErr state) noexcept;
    common::IntrusiveListHook queue_hook_{};
    std::size_t encoded_size;
    std::uint64_t window_consumed;
    void *ctx;
    EncodeCallback encode_cb;
    SendDoneCallback send_done_cb;

private:
    bool sending_;
    friend class Http2Connection;
    friend class Http2OutboundScheduler;
    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
