#include <fiber/http/ServerHttp2Request.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/Http2Connection.h>
#include <fiber/http/Http2DataFrameEncoder.h>
#include <fiber/http/Http2HeadersFrameEncoder.h>
#include <fiber/http/HttpUriParse.h>
#include <fiber/http/Huffman.h>

namespace fiber::http {

namespace {

HttpMethod parse_method(std::string_view method) noexcept {
    if (method == "GET") {
        return HttpMethod::Get;
    }
    if (method == "POST") {
        return HttpMethod::Post;
    }
    if (method == "PUT") {
        return HttpMethod::Put;
    }
    if (method == "DELETE") {
        return HttpMethod::Delete;
    }
    if (method == "HEAD") {
        return HttpMethod::Head;
    }
    if (method == "OPTIONS") {
        return HttpMethod::Options;
    }
    if (method == "PATCH") {
        return HttpMethod::Patch;
    }
    if (method == "CONNECT") {
        return HttpMethod::Connect;
    }
    if (method == "TRACE") {
        return HttpMethod::Trace;
    }
    return HttpMethod::Unknown;
}

bool is_pseudo_header(std::string_view name) noexcept { return !name.empty() && name.front() == ':'; }

} // namespace

struct ServerHttp2Request::SendResponseHeaderOp {
    using SuccessType = void;

    SendResponseHeaderOp(const OutgoingHeaderBlockView &header) noexcept :
        kind_(header.kind), headers_(header.headers), status_code_(header.status_code), reason_(header.reason),
        end_stream_(header.end_stream), informational_(header.kind == OutgoingHeaderKind::Informational) {}

    [[nodiscard]] common::IoErr submit(ServerHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Headers);
    }

    void on_send_done(ServerHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ServerHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

    OutgoingHeaderKind kind_ = OutgoingHeaderKind::Final;
    const HttpHeaders *headers_ = nullptr;
    int status_code_ = 0;
    std::string_view reason_;
    bool end_stream_ = false;
    bool informational_ = false;
};

struct ServerHttp2Request::SendResponseBodyAllOp {
    using SuccessType = std::size_t;

    explicit SendResponseBodyAllOp(mem::IoBufChain &&chunk) noexcept :
        chunk_(std::move(chunk)), total_bytes_(chunk_.readable_bytes()) {}

    [[nodiscard]] bool should_complete_without_submit() const noexcept {
        return chunk_.readable_bytes() == 0 && !chunk_.complete();
    }

    [[nodiscard]] common::IoErr submit(ServerHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Data);
    }

    [[nodiscard]] std::size_t pending_flow_controlled_bytes() const noexcept { return chunk_.readable_bytes(); }
    void on_send_done(ServerHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ServerHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

    [[nodiscard]] std::size_t success_result() const noexcept { return total_bytes_; }

    mem::IoBufChain chunk_;
    std::size_t total_bytes_ = 0;
};

struct ServerHttp2Request::SendResponseBodySomeOp {
    using SuccessType = std::size_t;
    inline static constexpr bool kAllowsPartialFinalBatch = true;

    explicit SendResponseBodySomeOp(mem::IoBufChain &chunk) noexcept :
        chunk_(&chunk), total_bytes_(chunk.readable_bytes()), end_(chunk.complete()) {}

    SendResponseBodySomeOp(const std::uint8_t *buf, std::size_t len, bool end) noexcept :
        buf_(buf), total_bytes_(len), end_(end) {}

    [[nodiscard]] bool should_complete_without_submit() const noexcept { return total_bytes_ == 0 && !end_; }

    [[nodiscard]] common::IoErr submit(ServerHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Data);
    }

    [[nodiscard]] std::size_t pending_flow_controlled_bytes() const noexcept { return total_bytes_; }
    void on_send_done(ServerHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ServerHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

    [[nodiscard]] std::size_t success_result() const noexcept { return accepted_bytes_; }

    mem::IoBufChain *chunk_ = nullptr;
    const std::uint8_t *buf_ = nullptr;
    std::size_t total_bytes_ = 0;
    std::size_t accepted_bytes_ = 0;
    bool end_ = false;
};

const Http2Stream::Ops &ServerHttp2Request::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
            &ServerHttp2Request::destroy_owner,
            &ServerHttp2Request::on_header_block_start,
            &ServerHttp2Request::on_header_block_complete,
            &ServerHttp2Request::on_body,
            &ServerHttp2Request::on_stream_abort,
    };
    return kOps;
}

const Http2HpackDecoder::Ops &ServerHttp2Request::decoder_ops() noexcept {
    static const Http2HpackDecoder::Ops kOps{
            &ServerHttp2Request::on_indexed_field, &ServerHttp2Request::on_indexed_name,
            &ServerHttp2Request::on_name_raw,      &ServerHttp2Request::on_name_huffman,
            &ServerHttp2Request::on_value_raw,     &ServerHttp2Request::on_value_huffman,
    };
    return kOps;
}

const HeaderMap<ServerHttp2Request::PseudoHeaderHandler> &ServerHttp2Request::pseudo_header_handler_map() noexcept {
    static HeaderMap<PseudoHeaderHandler> handlers = []() {
        HeaderMap<PseudoHeaderHandler>::Builder builder(5);
        builder.insert(":method", &ServerHttp2Request::handle_method);
        builder.insert(":path", &ServerHttp2Request::handle_path);
        builder.insert(":scheme", &ServerHttp2Request::handle_scheme);
        builder.insert(":authority", &ServerHttp2Request::handle_authority);
        builder.insert(":protocol", &ServerHttp2Request::handle_protocol);
        return std::move(builder).build();
    }();
    return handlers;
}

ServerHttp2Request::ServerHttp2Request(std::uint32_t stream_id, Http2Connection &conn,
                                       const HttpServerOptions &http_options, const HttpHandler &handler,
                                       std::shared_ptr<const HttpHandler> handler_owner) noexcept :
    conn_(&conn), handler_(&handler), handler_owner_(std::move(handler_owner)), stream_(this, stream_ops()),
    exchange_(conn.transport().loop().io_buf_node_pool(), http_options, conn.transport().remote_addr()),
    request_body_recv_(conn.transport().loop().io_buf_node_pool()) {
    (void) stream_id;
    FIBER_ASSERT(handler_ != nullptr);
    exchange_.request_body_spec_ = HttpBodySpec::Stream();
}

Http2Stream::Lease ServerHttp2Request::create(std::uint32_t stream_id, Http2Connection &conn,
                                              const HttpServerOptions &http_options,
                                              const HttpHandler &handler) noexcept {
    auto *owner = new (std::nothrow) ServerHttp2Request(stream_id, conn, http_options, handler);
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

Http2Stream::Lease ServerHttp2Request::create(std::uint32_t stream_id, Http2Connection &conn,
                                              const HttpServerOptions &http_options,
                                              std::shared_ptr<const HttpHandler> handler) noexcept {
    if (!handler) {
        return {};
    }
    const HttpHandler *handler_ptr = handler.get();
    auto *owner =
            new (std::nothrow) ServerHttp2Request(stream_id, conn, http_options, *handler_ptr, std::move(handler));
    if (!owner) {
        return {};
    }
    return Http2Stream::Lease::adopt(&owner->stream_);
}

common::IoErr ServerHttp2Request::on_header_block_start(void *owner, Http2HpackDecoder::Sink &sink) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (request->reading_trailers_ || request->exchange_.request_trailers_complete_) {
        return common::IoErr::Invalid;
    }
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    request->saw_regular_header_in_block_ = false;
    if (request->request_head_received_) {
        request->reading_trailers_ = true;
    }
    sink.ctx = request;
    sink.ops = &decoder_ops();
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_header_block_complete(void *owner, bool end_stream) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (!request->request_head_received_) {
        request->request_head_received_ = true;
        if (end_stream && request->exchange_.request_body_spec_.is_stream()) {
            request->exchange_.request_body_spec_ = HttpBodySpec::None();
        }
        if (!request->handler_started_) {
            request->exchange_.set_io(request);
            request->handler_started_ = true;
            fiber::async::spawn([request, lease = request->stream_.lease()]() mutable {
                return run_handler_task(request, std::move(lease));
            });
        }
        if (end_stream) {
            request->exchange_.request_trailers_complete_ = true;
            request->request_body_recv_.close_input();
        }
        return common::IoErr::None;
    }

    if (!request->reading_trailers_ || !end_stream) {
        return common::IoErr::Invalid;
    }

    request->reading_trailers_ = true;
    request->exchange_.request_trailers_complete_ = true;
    request->request_body_recv_.close_input();
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_body(void *owner, mem::IoBuf &&buf, bool end_stream) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (!request->request_head_received_ || request->reading_trailers_ ||
        request->exchange_.request_trailers_complete_) {
        return common::IoErr::Invalid;
    }
    if (request->discard_request_body_) {
        if (end_stream) {
            request->exchange_.request_trailers_complete_ = true;
        }
        return request->stream_.maybe_replenish_recv_window(0);
    }
    common::IoErr err = request->request_body_recv_.push_body(std::move(buf), end_stream);
    if (err != common::IoErr::None) {
        return err;
    }
    if (end_stream) {
        request->exchange_.request_trailers_complete_ = true;
    }
    return common::IoErr::None;
}

void ServerHttp2Request::on_stream_abort(void *owner, common::IoErr reason) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    request->on_stream_aborted(reason);
}

void ServerHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ServerHttp2Request *>(owner); }

fiber::async::DetachedTask ServerHttp2Request::run_handler_task(ServerHttp2Request *request,
                                                                Http2Stream::Lease lease) noexcept {
    if (!request || !request->handler_) {
        co_return;
    }

    co_await (*request->handler_)(request->exchange_);
    request->handler_done_ = true;

    if (request->response_finished_ && !request->stream_.remote_end_stream() && !request->stream_.local_rst() &&
        !request->stream_.remote_rst()) {
        request->discard_request_body_ = true;
        request->request_body_recv_.discard_buffered();
        const common::IoErr err = request->stream_.maybe_replenish_recv_window(0);
        if (err != common::IoErr::None) {
            (void) request->stream_.close_rst(Http2ErrorCode::InternalError, err);
        }
    }
    request->exchange_.set_io(nullptr);

    if (!request->stream_.local_end_stream() && !request->stream_.local_rst() && !request->stream_.remote_rst() &&
        request->stream_.close_reason() == common::IoErr::None) {
        (void) request->stream_.close_rst(Http2ErrorCode::Cancel, common::IoErr::NotSupported);
    }

    (void) lease;
    co_return;
}

common::IoErr ServerHttp2Request::SendResponseHeaderOp::on_encode(ServerHttp2Request &request, Http2Stream &stream,
                                                                  const Http2OutboundEncodeRequest &req,
                                                                  Http2OutboundEncodeTarget &target,
                                                                  Http2OutboundEncodeResult &result) noexcept {
    if (request.abort_reason_ != common::IoErr::None || request.stream_.local_rst() || request.stream_.remote_rst()) {
        return request.abort_reason_ != common::IoErr::None ? request.abort_reason_ : common::IoErr::Canceled;
    }

    Http2HeadersFrameEncoder frame_encoder({
            .stream_id = stream.stream_id(),
            .max_frame_size = req.max_frame_size,
            .end_stream = end_stream_,
            .hpack = {.max_string_size = request.conn_->options_.max_hpack_string_size},
    });
    common::IoErr err = frame_encoder.begin(target);
    if (err != common::IoErr::None) {
        return err;
    }

    if (kind_ != OutgoingHeaderKind::Trailer) {
        err = frame_encoder.encode_status(status_code_);
        if (err != common::IoErr::None) {
            frame_encoder.abort();
            return err;
        }
    }

    if (headers_ != nullptr) {
        for (auto it = headers_->begin(); it != headers_->end(); ++it) {
            const auto &field = *it;
            if (field.name_len == 0) {
                continue;
            }
            std::string_view lowcase_name = field.lowcase_view();
            if (lowcase_name.empty()) {
                lowcase_name = field.name_view();
            }
            err = frame_encoder.encode_field(lowcase_name, field.name_hash, field.value_view());
            if (err != common::IoErr::None) {
                frame_encoder.abort();
                return err;
            }
        }
    }

    err = frame_encoder.finish();
    if (err != common::IoErr::None) {
        return err;
    }

    result.flow_controlled_bytes = 0;
    result.operation_final_batch = true;
    return common::IoErr::None;
}

void ServerHttp2Request::SendResponseHeaderOp::on_send_done(ServerHttp2Request &request,
                                                            std::uint32_t flow_controlled_bytes,
                                                            bool operation_final_batch) noexcept {
    FIBER_ASSERT(flow_controlled_bytes == 0);
    FIBER_ASSERT(operation_final_batch);
    if (end_stream_) {
        request.stream_.local_end_stream_ = true;
        request.response_finished_ = true;
    }
    if (kind_ == OutgoingHeaderKind::Final) {
        request.response_headers_sent_ = true;
        request.response_finished_ = end_stream_;
        request.response_status_code_ = status_code_;
        request.response_reason_ = reason_;
        request.response_headers_ = headers_;
    }
}

common::IoErr ServerHttp2Request::SendResponseBodyAllOp::on_encode(ServerHttp2Request &request, Http2Stream &stream,
                                                                   const Http2OutboundEncodeRequest &req,
                                                                   Http2OutboundEncodeTarget &target,
                                                                   Http2OutboundEncodeResult &result) noexcept {
    if (request.abort_reason_ != common::IoErr::None || request.stream_.local_rst() || request.stream_.remote_rst()) {
        return request.abort_reason_ != common::IoErr::None ? request.abort_reason_ : common::IoErr::Canceled;
    }

    const std::size_t remaining = chunk_.readable_bytes();
    if (remaining == 0) {
        FIBER_ASSERT(chunk_.complete());

        Http2DataFrameEncoder frame_encoder({
                .stream_id = stream.stream_id(),
                .max_frame_size = req.max_frame_size,
                .end_stream = true,
        });
        common::IoErr err = frame_encoder.encode(target, chunk_, 0);
        if (err != common::IoErr::None) {
            return err;
        }
        result.flow_controlled_bytes = 0;
        result.operation_final_batch = true;
        return common::IoErr::None;
    }

    FIBER_ASSERT(req.payload_budget != 0);
    const std::size_t payload_budget = std::min<std::size_t>(remaining, req.payload_budget);
    Http2DataFrameEncoder frame_encoder({
            .stream_id = stream.stream_id(),
            .max_frame_size = req.max_frame_size,
            .end_stream = chunk_.complete() && payload_budget == remaining,
    });
    common::IoErr err = frame_encoder.encode(target, chunk_, payload_budget);
    if (err != common::IoErr::None) {
        return err;
    }

    const std::size_t after_remaining = chunk_.readable_bytes();
    result.flow_controlled_bytes = static_cast<std::uint32_t>(payload_budget);
    result.operation_final_batch = after_remaining == 0;
    return common::IoErr::None;
}

void ServerHttp2Request::SendResponseBodyAllOp::on_send_done(ServerHttp2Request &request,
                                                             std::uint32_t flow_controlled_bytes,
                                                             bool operation_final_batch) noexcept {
    request.response_body_sent_ += flow_controlled_bytes;
    if (!operation_final_batch) {
        return;
    }
    if (chunk_.complete()) {
        request.stream_.local_end_stream_ = true;
        request.response_finished_ = true;
    }
}

common::IoErr ServerHttp2Request::SendResponseBodySomeOp::on_encode(ServerHttp2Request &request, Http2Stream &stream,
                                                                    const Http2OutboundEncodeRequest &req,
                                                                    Http2OutboundEncodeTarget &target,
                                                                    Http2OutboundEncodeResult &result) noexcept {
    if (request.abort_reason_ != common::IoErr::None || request.stream_.local_rst() || request.stream_.remote_rst()) {
        return request.abort_reason_ != common::IoErr::None ? request.abort_reason_ : common::IoErr::Canceled;
    }

    if (total_bytes_ == 0) {
        FIBER_ASSERT(end_);
        mem::IoBufChain empty(request.conn_->transport().loop().io_buf_node_pool());
        Http2DataFrameEncoder frame_encoder({
                .stream_id = stream.stream_id(),
                .max_frame_size = req.max_frame_size,
                .end_stream = true,
        });
        common::IoErr err = frame_encoder.encode(target, empty, 0);
        if (err != common::IoErr::None) {
            return err;
        }
        result.flow_controlled_bytes = 0;
        result.operation_final_batch = true;
        return common::IoErr::None;
    }

    FIBER_ASSERT(req.payload_budget != 0);
    const std::size_t payload_bytes = std::min(total_bytes_, static_cast<std::size_t>(req.payload_budget));
    mem::IoBufChain staged;
    mem::IoBufChain *payload = chunk_;
    bool consume_borrowed_chain = false;

    if (chunk_ == nullptr) {
        staged.bind_node_pool(request.conn_->transport().loop().io_buf_node_pool());
        mem::IoBuf owned = mem::IoBuf::allocate(payload_bytes);
        if (!owned) {
            return common::IoErr::NoMem;
        }
        std::memcpy(owned.writable_data(), buf_, payload_bytes);
        owned.commit(payload_bytes);
        if (!staged.append(std::move(owned))) {
            return common::IoErr::NoMem;
        }
        if (end_ && payload_bytes == total_bytes_) {
            staged.mark_complete();
        }
        payload = &staged;
    } else if (&chunk_->node_pool() != &request.conn_->transport().loop().io_buf_node_pool()) {
        staged.bind_node_pool(request.conn_->transport().loop().io_buf_node_pool());
        if (!chunk_->retain_prefix(payload_bytes, staged)) {
            return common::IoErr::NoMem;
        }
        payload = &staged;
        consume_borrowed_chain = true;
    }

    Http2DataFrameEncoder frame_encoder({
            .stream_id = stream.stream_id(),
            .max_frame_size = req.max_frame_size,
            .end_stream = end_ && payload_bytes == total_bytes_,
    });
    common::IoErr err = frame_encoder.encode(target, *payload, payload_bytes);
    if (err != common::IoErr::None) {
        return err;
    }
    if (consume_borrowed_chain) {
        chunk_->consume_and_compact(payload_bytes);
        if (payload_bytes == total_bytes_ && end_) {
            chunk_->clear_complete();
        }
    } else if (chunk_ != nullptr && payload_bytes == total_bytes_ && end_) {
        chunk_->clear_complete();
    }

    accepted_bytes_ = payload_bytes;
    result.flow_controlled_bytes = static_cast<std::uint32_t>(payload_bytes);
    result.operation_final_batch = true;
    return common::IoErr::None;
}

void ServerHttp2Request::SendResponseBodySomeOp::on_send_done(ServerHttp2Request &request,
                                                              std::uint32_t flow_controlled_bytes,
                                                              bool operation_final_batch) noexcept {
    FIBER_ASSERT(operation_final_batch);
    FIBER_ASSERT(flow_controlled_bytes == accepted_bytes_);
    request.response_body_sent_ += flow_controlled_bytes;
    if (end_ && accepted_bytes_ == total_bytes_) {
        request.stream_.local_end_stream_ = true;
        request.response_finished_ = true;
    }
}

bool ServerHttp2Request::cancel_queued_send() noexcept {
    if (abort_reason_ != common::IoErr::None) {
        return false;
    }
    if (conn_ == nullptr) {
        return false;
    }
    return conn_->cancel_queued_stream_send(stream_);
}

void ServerHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    mark_response_channel_closed();
    request_body_recv_.abort(abort_reason_);
}

common::IoErr ServerHttp2Request::set_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                       void *ctx) noexcept {
    if (callback == nullptr) {
        return common::IoErr::Invalid;
    }
    if (response_channel_closed_callback_ != nullptr) {
        return common::IoErr::Busy;
    }
    if (response_channel_closed_) {
        callback(ctx);
        return common::IoErr::None;
    }
    response_channel_closed_callback_ = callback;
    response_channel_closed_callback_ctx_ = ctx;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::clear_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                         void *ctx) noexcept {
    if (callback == nullptr) {
        return common::IoErr::Invalid;
    }
    if (response_channel_closed_callback_ == callback && response_channel_closed_callback_ctx_ == ctx) {
        response_channel_closed_callback_ = nullptr;
        response_channel_closed_callback_ctx_ = nullptr;
    }
    return common::IoErr::None;
}

void ServerHttp2Request::mark_response_channel_closed() noexcept {
    if (response_channel_closed_) {
        return;
    }
    response_channel_closed_ = true;
    ResponseChannelClosedCallback callback = response_channel_closed_callback_;
    void *ctx = response_channel_closed_callback_ctx_;
    response_channel_closed_callback_ = nullptr;
    response_channel_closed_callback_ctx_ = nullptr;
    if (callback != nullptr) {
        callback(ctx);
    }
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
ServerHttp2Request::read_body(HttpExchange &, std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    co_return co_await request_body_recv_.read_body(stream_, max_bytes, timeout);
}

fiber::async::Task<common::IoResult<void>> ServerHttp2Request::send_header(HttpExchange &exchange,
                                                                           const OutgoingHeaderBlockView &header,
                                                                           std::chrono::milliseconds timeout) {
    if (&exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!handler_started_ || stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (header.kind != OutgoingHeaderKind::Trailer) {
        const bool informational = header.kind == OutgoingHeaderKind::Informational;
        if (header.status_code < 100 || header.status_code > 999) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (informational && (header.status_code < 100 || header.status_code >= 200)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (!informational && header.status_code >= 100 && header.status_code < 200) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (!informational && header.body.is_chunked()) {
            co_return std::unexpected(common::IoErr::NotSupported);
        }
    }

    co_return co_await HeaderSendAwaiter(*this, timeout, header);
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write_all(HttpExchange &exchange,
                                                                           mem::IoBufChain chunk,
                                                                           std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr || &exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    co_return co_await BodyWriteAllAwaiter(*this, timeout, std::move(chunk));
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write_all(HttpExchange &exchange,
                                                                           const std::uint8_t *buf, std::size_t len,
                                                                           bool end,
                                                                           std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    mem::IoBufChain chunk(conn_->transport().loop().io_buf_node_pool());
    if (end) {
        chunk.mark_complete();
    }
    if (len != 0) {
        mem::IoBuf owned = mem::IoBuf::allocate(len);
        if (!owned) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(owned.writable_data(), buf, len);
        owned.commit(len);
        if (!chunk.append(std::move(owned))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }

    co_return co_await write_all(exchange, std::move(chunk), timeout);
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write(HttpExchange &exchange, mem::IoBufChain &chunk,
                                                                       std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr || &exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    co_return co_await BodyWriteSomeAwaiter(*this, timeout, chunk);
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write(HttpExchange &exchange, const std::uint8_t *buf,
                                                                       std::size_t len, bool end,
                                                                       std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (conn_ == nullptr || &exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    co_return co_await BodyWriteSomeAwaiter(*this, timeout, buf, len, end);
}

common::IoResult<void> ServerHttp2Request::abort(HttpExchange &exchange, common::IoErr reason) noexcept {
    if (&exchange != &exchange_ || conn_ == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const common::IoErr err = stream_.close_rst(Http2ErrorCode::Cancel, reason);
    if (err != common::IoErr::None && err != common::IoErr::Canceled) {
        return std::unexpected(err);
    }
    return {};
}

common::IoErr ServerHttp2Request::on_indexed_field(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    const bool stable = entry.stable_for_exchange();
    return request->commit_field(entry.name, entry.name_hash, entry.value, stable, stable);
}

common::IoErr ServerHttp2Request::on_indexed_name(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    request->pending_name_ = entry.name;
    request->pending_name_hash_ = entry.name_hash;
    request->pending_name_stable_ = entry.stable_for_exchange();
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    std::string_view name;
    std::uint64_t name_hash = 0;
    common::IoErr err = request->materialize_name_raw(data, len, name, name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    request->pending_name_ = name;
    request->pending_name_hash_ = name_hash;
    request->pending_name_stable_ = true;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    std::string_view name;
    std::uint64_t name_hash = 0;
    common::IoErr err = request->materialize_name_huffman(data, len, name, name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    request->pending_name_ = name;
    request->pending_name_hash_ = name_hash;
    request->pending_name_stable_ = true;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                               Http2HpackDecoder::FieldView *out) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    std::string_view value;
    common::IoErr err = request->materialize_value_raw(data, len, value);
    if (err != common::IoErr::None) {
        return err;
    }
    if (request->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    std::uint64_t pending_name_hash = request->pending_name_hash_;
    std::string_view pending_name = request->pending_name_;
    bool pending_name_stable = request->pending_name_stable_;
    if (out != nullptr && !pending_name_stable) {
        pending_name = request->copy_to_pool(pending_name);
        if (!pending_name.data() && !request->pending_name_.empty()) {
            return common::IoErr::NoMem;
        }
        pending_name_stable = true;
        out->name = pending_name;
        out->name_hash = pending_name_hash;
        out->value = value;
    } else if (out != nullptr) {
        out->name = pending_name;
        out->name_hash = pending_name_hash;
        out->value = value;
    }
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    return request->commit_field(pending_name, pending_name_hash, value, pending_name_stable, true);
}

common::IoErr ServerHttp2Request::on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                   Http2HpackDecoder::FieldView *out) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    std::string_view value;
    common::IoErr err = request->materialize_value_huffman(data, len, value);
    if (err != common::IoErr::None) {
        return err;
    }
    if (request->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    std::uint64_t pending_name_hash = request->pending_name_hash_;
    std::string_view pending_name = request->pending_name_;
    bool pending_name_stable = request->pending_name_stable_;
    if (out != nullptr && !pending_name_stable) {
        pending_name = request->copy_to_pool(pending_name);
        if (!pending_name.data() && !request->pending_name_.empty()) {
            return common::IoErr::NoMem;
        }
        pending_name_stable = true;
        out->name = pending_name;
        out->name_hash = pending_name_hash;
        out->value = value;
    } else if (out != nullptr) {
        out->name = pending_name;
        out->name_hash = pending_name_hash;
        out->value = value;
    }
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    return request->commit_field(pending_name, pending_name_hash, value, pending_name_stable, true);
}

common::IoErr ServerHttp2Request::materialize_name_raw(const std::uint8_t *data, std::size_t len, std::string_view &out,
                                                       std::uint64_t &name_hash) noexcept {
    std::string_view name = copy_to_pool(data, len);
    if (!name.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    out = name;
    name_hash = http_header_name_hash(name);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                           std::string_view &out, std::uint64_t &name_hash) noexcept {
    out = {};
    bool ok = false;
    std::size_t decoded_len = hpack_huffman_decoded_length(data, len, &ok);
    if (!ok) {
        return common::IoErr::Invalid;
    }
    if (decoded_len != 0) {
        auto *mem = static_cast<char *>(exchange_.pool_.alloc(decoded_len));
        if (!mem) {
            return common::IoErr::NoMem;
        }

        HpackHuffmanDecodeState state;
        HpackHuffmanDecodeResult result =
                hpack_huffman_decode_exact(state, data, len, reinterpret_cast<std::uint8_t *>(mem), true);
        if (result.code != HpackHuffmanCode::Ok || result.written != decoded_len) {
            return common::IoErr::Invalid;
        }

        out = std::string_view(mem, decoded_len);
    }
    name_hash = http_header_name_hash(out);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                        std::string_view &out) noexcept {
    out = copy_to_pool(data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::materialize_value_huffman(const std::uint8_t *data, std::size_t len,
                                                            std::string_view &out) noexcept {
    out = {};
    bool ok = false;
    std::size_t decoded_len = hpack_huffman_decoded_length(data, len, &ok);
    if (!ok) {
        return common::IoErr::Invalid;
    }
    if (decoded_len == 0) {
        return common::IoErr::None;
    }

    auto *mem = static_cast<char *>(exchange_.pool_.alloc(decoded_len));
    if (!mem) {
        return common::IoErr::NoMem;
    }

    HpackHuffmanDecodeState state;
    HpackHuffmanDecodeResult result =
            hpack_huffman_decode_exact(state, data, len, reinterpret_cast<std::uint8_t *>(mem), true);
    if (result.code != HpackHuffmanCode::Ok || result.written != decoded_len) {
        return common::IoErr::Invalid;
    }

    out = std::string_view(mem, decoded_len);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                               bool name_stable, bool value_stable) noexcept {
    if (is_pseudo_header(name)) {
        if (reading_trailers_ || saw_regular_header_in_block_) {
            return common::IoErr::Invalid;
        }

        auto *handler = pseudo_header_handler_map().get(name);
        if (!handler) {
            return common::IoErr::Invalid;
        }
        return (*handler)(*this, value, value_stable);
    }

    saw_regular_header_in_block_ = true;
    return commit_regular_header(name, name_hash, value, name_stable, value_stable);
}

common::IoErr ServerHttp2Request::commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                        std::string_view value, bool name_stable,
                                                        bool value_stable) noexcept {
    if (!reading_trailers_) {
        common::IoErr err = apply_regular_header_policy(name, name_hash, value);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    std::string_view name_copy = name_stable ? name : copy_to_pool(name);
    std::string_view value_copy = value_stable ? value : copy_to_pool(value);
    if ((!name_copy.data() && !name.empty()) || (!value_copy.data() && !value.empty())) {
        return common::IoErr::NoMem;
    }

    HttpHeaders &target = reading_trailers_ ? exchange_.request_trailers_ : exchange_.request_headers_;
    char *lowcase_name = name_copy.empty() ? nullptr : const_cast<char *>(name_copy.data());
    auto *field = target.add_view(name_copy, value_copy, lowcase_name, name_hash);
    if (!field) {
        return common::IoErr::NoMem;
    }
    if (!reading_trailers_) {
        exchange_.cache_request_header_field(*field);
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::apply_regular_header_policy(std::string_view name, std::uint64_t name_hash,
                                                              std::string_view value) noexcept {
    static constexpr std::uint64_t kContentLengthHash = http_header_name_hash("content-length");
    static constexpr std::uint64_t kTransferEncodingHash = http_header_name_hash("transfer-encoding");
    if (name_hash == kTransferEncodingHash && name == "transfer-encoding") {
        return common::IoErr::Invalid;
    }
    if (name_hash != kContentLengthHash || name != "content-length") {
        return common::IoErr::None;
    }

    unsigned long long parsed = 0;
    const char *first = value.data();
    const char *last = value.data() + value.size();
    const auto result = std::from_chars(first, last, parsed, 10);
    if (result.ec != std::errc() || result.ptr != last ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return common::IoErr::Invalid;
    }

    const std::size_t length = static_cast<std::size_t>(parsed);
    if (exchange_.request_body_spec_.is_content_length() && exchange_.request_body_spec_.content_length() != length) {
        return common::IoErr::Invalid;
    }
    exchange_.request_body_spec_ = HttpBodySpec::ContentLength(length);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_method(ServerHttp2Request &request, std::string_view value,
                                                bool value_stable) noexcept {
    std::string_view method = value_stable ? value : request.copy_to_pool(value);
    if (method.data() == nullptr && !method.empty()) {
        return common::IoErr::NoMem;
    }
    request.exchange_.method_view_ = method;
    request.exchange_.method_ = parse_method(method);
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_path(ServerHttp2Request &request, std::string_view value,
                                              bool value_stable) noexcept {
    std::string_view path = value_stable ? value : request.copy_to_pool(value);
    if (path.data() == nullptr && !path.empty()) {
        return common::IoErr::NoMem;
    }
    HttpUriParseState uri_state{};
    common::IoErr err = http_parse_uri(path, uri_state);
    if (err != common::IoErr::None) {
        return err;
    }
    err = http_process_uri(path, uri_state, request.exchange_.uri_, &request.exchange_.pool());
    if (err != common::IoErr::None) {
        return err;
    }
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_scheme(ServerHttp2Request &request, std::string_view value,
                                                bool value_stable) noexcept {
    std::string_view scheme = value_stable ? value : request.copy_to_pool(value);
    if (scheme.data() == nullptr && !scheme.empty()) {
        return common::IoErr::NoMem;
    }
    request.exchange_.scheme_view_ = scheme;
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_authority(ServerHttp2Request &request, std::string_view value,
                                                   bool value_stable) noexcept {
    return request.commit_regular_header("host", http_header_name_hash("host"), value, true, value_stable);
}

common::IoErr ServerHttp2Request::handle_protocol(ServerHttp2Request &request, std::string_view value,
                                                  bool value_stable) noexcept {
    if (request.protocol_seen_ || request.conn_ == nullptr || !request.conn_->local_enable_connect_protocol()) {
        return common::IoErr::Invalid;
    }
    std::string_view protocol = value_stable ? value : request.copy_to_pool(value);
    if (protocol.data() == nullptr && !protocol.empty()) {
        return common::IoErr::NoMem;
    }
    request.protocol_seen_ = true;
    request.exchange_.protocol_view_ = protocol;
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

std::string_view ServerHttp2Request::copy_to_pool(const std::uint8_t *data, std::size_t len) noexcept {
    if (len == 0) {
        return {};
    }
    auto *mem = static_cast<char *>(exchange_.pool_.alloc(len));
    if (!mem) {
        return {};
    }
    std::memcpy(mem, data, len);
    return std::string_view(mem, len);
}

std::string_view ServerHttp2Request::copy_to_pool(std::string_view value) noexcept {
    return copy_to_pool(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}

} // namespace fiber::http
