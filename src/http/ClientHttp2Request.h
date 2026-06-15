#ifndef FIBER_HTTP_CLIENT_HTTP2_REQUEST_H
#define FIBER_HTTP_CLIENT_HTTP2_REQUEST_H

#include <chrono>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "ClientHttp2Types.h"
#include "Http2HpackDecoder.h"
#include "Http2Stream.h"
#include "Http2StreamFactory.h"
#include "detail/Http2BodyRecvState.h"
#include "detail/Http2HeaderBlockQueue.h"
#include "detail/Http2SendAwaiter.h"

namespace fiber::http {

class Http2Connection;
class Http2OutboundEncodeTarget;
class HttpHeaders;
struct Http2OutboundEncodeRequest;
struct Http2OutboundEncodeResult;
struct BodyChunk;

class ClientHttp2Request : public common::NonCopyable, public common::NonMovable {
public:
    ~ClientHttp2Request() = default;

    [[nodiscard]] static const Http2StreamFactoryOps &factory_ops() noexcept;
    [[nodiscard]] static ClientHttp2Request *create(Http2Connection &conn, mem::BufPool &pool) noexcept;

    fiber::async::Task<common::IoResult<void>> send_request_header(const Http2RequestHead &head,
                                                                   bool end_stream) noexcept;
    fiber::async::Task<common::IoResult<std::size_t>> write_body(BodyChunk chunk) noexcept;
    fiber::async::Task<common::IoResult<void>> write_trailer(const HttpHeaders &headers) noexcept;
    fiber::async::Task<common::IoResult<const Http2ResponseHead *>> read_header() noexcept;
    fiber::async::Task<common::IoResult<BodyChunk>> read_body(std::size_t max_bytes) noexcept;

    [[nodiscard]] Http2Stream &stream() noexcept { return stream_; }
    [[nodiscard]] const Http2Stream &stream() const noexcept { return stream_; }
    [[nodiscard]] mem::IoBufNodePool &node_pool() noexcept;

private:
    struct SendRequestHeaderOp;
    struct SendRequestBodyOp;
    struct SendRequestTrailerOp;
    using SendAwaiter = detail::SendAwaiterBase<ClientHttp2Request>;
    using HeaderSendAwaiter = detail::HeaderSendAwaiter<ClientHttp2Request, SendRequestHeaderOp>;
    using BodySendAwaiter = detail::BodySendAwaiter<ClientHttp2Request, SendRequestBodyOp>;
    using TrailerSendAwaiter = detail::HeaderSendAwaiter<ClientHttp2Request, SendRequestTrailerOp>;

    static Http2Stream::Lease create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    static Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id, Http2Connection &conn) noexcept;
    explicit ClientHttp2Request(Http2Connection &conn, mem::BufPool &pool) noexcept;
    static const Http2Stream::Ops &stream_ops() noexcept;
    static const Http2HpackDecoder::Ops &decoder_ops() noexcept;
    static common::IoErr on_header_block_start(void *owner, Http2HpackDecoder::Sink &sink) noexcept;
    static common::IoErr on_header_block_complete(void *owner, bool end_stream) noexcept;
    static common::IoErr on_body(void *owner, mem::IoBuf &&buf, bool end_stream) noexcept;
    static common::IoErr on_indexed_field(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept;
    static common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept;
    static common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                      Http2HpackDecoder::FieldView *out) noexcept;
    static common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                          Http2HpackDecoder::FieldView *out) noexcept;
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
    void on_send_complete(SendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_header_send_complete(HeaderSendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_body_send_complete(BodySendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_trailer_send_complete(TrailerSendAwaiter *awaiter, common::IoErr result) noexcept;
    void on_stream_send_window_available() noexcept;
    void on_stream_aborted(common::IoErr reason) noexcept;
    [[nodiscard]] common::IoErr handle_status(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                             bool name_owned = false) noexcept;
    [[nodiscard]] common::IoErr commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                      std::string_view value, bool name_owned = false) noexcept;
    [[nodiscard]] common::IoErr materialize_name_raw(const std::uint8_t *data, std::size_t len, std::string_view &out,
                                                     std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                         std::string_view &out, std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                      std::string_view &out) noexcept;
    [[nodiscard]] common::IoErr materialize_value_huffman(const std::uint8_t *data, std::size_t len,
                                                          std::string_view &out) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(std::string_view value) noexcept;

    Http2Connection *conn_ = nullptr;
    Http2Stream stream_;
    mem::BufPool *pool_ = nullptr;
    detail::Http2BodyRecvState response_body_recv_;
    detail::Http2HeaderBlockQueue response_header_recv_;
    SendAwaiter *send_awaiter_ = nullptr;
    common::IoErr abort_reason_ = common::IoErr::None;
    bool request_headers_sent_ = false;
    bool request_finished_ = false;
    bool response_head_received_ = false;
    bool reading_trailers_ = false;
    bool current_block_has_status_ = false;
    bool saw_regular_header_in_block_ = false;
    detail::Http2HeaderBlockQueue::HeaderNode *current_header_node_ = nullptr;
    std::string_view pending_name_{};
    std::uint64_t pending_name_hash_ = 0;
    bool pending_name_owned_ = false;

    template<class>
    friend class detail::SendAwaiterBase;
    template<class, class>
    friend class detail::HeaderSendAwaiter;
    template<class, class>
    friend class detail::BodySendAwaiter;
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_REQUEST_H
