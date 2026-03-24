#ifndef FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
#define FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H

#include <cstdint>

#include "../common/IntrusiveList.h"
#include "../common/IoError.h"

namespace fiber::http {

class Http2Stream;
class Http2OutboundScheduler;
class Http2OutboundPayloadStorage;
struct Http2OutboundEncodeRequest;
struct Http2OutboundEncodeResult;

enum class Http2OutboundNextKind : std::uint8_t {
    None = 0,
    Headers,
    Data,
};

using Http2OutboundEncodeFn = common::IoErr (*)(Http2Stream &stream, void *ctx,
                                                const Http2OutboundEncodeRequest &req,
                                                Http2OutboundPayloadStorage &storage,
                                                Http2OutboundEncodeResult &result) noexcept;

class Http2OutboundHook {
private:
    common::IntrusiveListHook queue_hook_{};
    Http2OutboundEncodeFn encode_ = nullptr;
    void *encode_ctx_ = nullptr;
    Http2OutboundNextKind next_kind_ = Http2OutboundNextKind::None;
    std::uint8_t queue_state_ = 0;
    bool closed_ = false;

    friend class Http2OutboundScheduler;
    friend class Http2Stream;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_OUTBOUND_HOOK_H
