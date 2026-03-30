#ifndef FIBER_HTTP_CLIENT_HTTP2_REQUEST_H
#define FIBER_HTTP_CLIENT_HTTP2_REQUEST_H

#include <chrono>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "Http2StreamFactory.h"
#include "Http2Stream.h"

namespace fiber::http {

class Http2Connection;
class Http2OutboundEncodeTarget;
class HttpHeaders;
struct Http2OutboundEncodeRequest;
struct Http2OutboundEncodeResult;
struct Http2RequestHead;
struct BodyChunk;

class ClientHttp2Request : public common::NonCopyable, public common::NonMovable {
public:
    ~ClientHttp2Request() = default;

    [[nodiscard]] static const Http2StreamFactoryOps &factory_ops() noexcept;
    [[nodiscard]] static ClientHttp2Request *create(Http2Connection &conn) noexcept;

    fiber::async::Task<common::IoResult<void>> send_request_header(const Http2RequestHead &head,
                                                                   bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<void>> write_trailer(const HttpHeaders &headers) noexcept;

    [[nodiscard]] Http2Stream &stream() noexcept { return stream_; }
    [[nodiscard]] const Http2Stream &stream() const noexcept { return stream_; }

private:
    class SendAwaiter;
    class HeaderSendAwaiter;
    class BodySendAwaiter;
    class TrailerSendAwaiter;

    static Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                    Http2Connection &conn) noexcept;
    explicit ClientHttp2Request(Http2Connection &conn) noexcept;
    static const Http2Stream::Ops &stream_ops() noexcept;
    static common::IoErr encode_request_frames(Http2Stream &stream, void *ctx, const Http2OutboundEncodeRequest &req,
                                               Http2OutboundEncodeTarget &target,
                                               Http2OutboundEncodeResult &result) noexcept;
    static common::IoErr encode_body_frames(Http2Stream &stream, void *ctx, const Http2OutboundEncodeRequest &req,
                                            Http2OutboundEncodeTarget &target,
                                            Http2OutboundEncodeResult &result) noexcept;
    static common::IoErr encode_trailer_frames(Http2Stream &stream, void *ctx, const Http2OutboundEncodeRequest &req,
                                               Http2OutboundEncodeTarget &target,
                                               Http2OutboundEncodeResult &result) noexcept;
    static void on_stream_abort(void *owner, common::IoErr reason) noexcept;
    static void on_stream_send_window_available(void *owner) noexcept;
    static void destroy_owner(void *owner) noexcept;
    [[nodiscard]] bool cancel_queued_send() noexcept;
    void on_header_send_complete(HeaderSendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_body_send_complete(BodySendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_trailer_send_complete(TrailerSendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_stream_send_window_available() noexcept;
    void on_stream_aborted(common::IoErr reason) noexcept;

    Http2Connection *conn_ = nullptr;
    Http2Stream stream_;
    mem::BufPool pool_;
    SendAwaiter *send_awaiter_ = nullptr;
    common::IoErr abort_reason_ = common::IoErr::None;
    bool request_headers_sent_ = false;
    bool request_finished_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_REQUEST_H
