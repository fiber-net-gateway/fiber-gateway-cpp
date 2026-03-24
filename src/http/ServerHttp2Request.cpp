#include "ServerHttp2Request.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <new>

#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "Http2Connection.h"
#include "Http2HeadersFrameEncoder.h"
#include "Http2HpackHuffman.h"

namespace fiber::http {

namespace {

constexpr std::string_view kConnectionHeader = "connection";
constexpr std::string_view kKeepAliveHeader = "keep-alive";
constexpr std::string_view kProxyConnectionHeader = "proxy-connection";
constexpr std::string_view kTransferEncodingHeader = "transfer-encoding";
constexpr std::string_view kUpgradeHeader = "upgrade";
constexpr std::string_view kContentLengthHeader = "content-length";

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

bool is_pseudo_header(std::string_view name) noexcept {
    return !name.empty() && name.front() == ':';
}

bool is_forbidden_http2_response_header(std::string_view lowcase_name) noexcept {
    return lowcase_name == kConnectionHeader || lowcase_name == kKeepAliveHeader ||
           lowcase_name == kProxyConnectionHeader || lowcase_name == kTransferEncodingHeader ||
           lowcase_name == kUpgradeHeader;
}

std::string_view format_content_length(std::uint64_t value, std::array<char, 20> &scratch) noexcept {
    char *out = scratch.data() + scratch.size();
    do {
        *--out = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0);
    return {out, static_cast<std::size_t>(scratch.data() + scratch.size() - out)};
}

} // namespace

struct ServerHttp2Request::BodyReadPollResult {
    enum class Kind : std::uint8_t {
        Wait,
        Readable,
        End,
        TimedOut,
        Closed,
    };

    Kind kind = Kind::Wait;
    common::IoErr error = common::IoErr::None;
};

class ServerHttp2Request::BodyReadAwaiter {
public:
    BodyReadAwaiter(ServerHttp2Request &request, std::chrono::milliseconds timeout) noexcept :
        request_(&request), timeout_(timeout) {}

    BodyReadAwaiter(const BodyReadAwaiter &) = delete;
    BodyReadAwaiter &operator=(const BodyReadAwaiter &) = delete;
    BodyReadAwaiter(BodyReadAwaiter &&) = delete;
    BodyReadAwaiter &operator=(BodyReadAwaiter &&) = delete;

    ~BodyReadAwaiter() {
        if (!request_) {
            return;
        }
        request_->cancel_body_waiter(this);
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<BodyReadAwaiter, &BodyReadAwaiter::timer_entry_>(*this);
        }
    }

    bool await_ready() noexcept {
        if (!request_) {
            return true;
        }
        BodyReadPollResult state = request_->poll_body_read_state();
        if (state.kind != BodyReadPollResult::Kind::Wait) {
            return true;
        }
        if (timeout_.count() == 0) {
            timed_out_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (!request_) {
            return false;
        }

        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        if (!request_->arm_body_waiter(this)) {
            return false;
        }
        if (has_timer()) {
            loop_->post_at<BodyReadAwaiter, &BodyReadAwaiter::timer_entry_, &BodyReadAwaiter::on_timeout>(
                loop_->now() + timeout_, *this);
        }
        return true;
    }

    BodyReadPollResult await_resume() noexcept {
        BodyReadPollResult result{};
        if (!request_) {
            result.kind = BodyReadPollResult::Kind::Closed;
            result.error = common::IoErr::Canceled;
            return result;
        }

        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<BodyReadAwaiter, &BodyReadAwaiter::timer_entry_>(*this);
        }
        if (request_->body_waiter_ == this) {
            request_->body_waiter_ = nullptr;
        }

        if (timed_out_) {
            result.kind = BodyReadPollResult::Kind::TimedOut;
        } else {
            result = request_->poll_body_read_state();
            FIBER_ASSERT(result.kind != BodyReadPollResult::Kind::Wait);
        }

        request_ = nullptr;
        handle_ = {};
        loop_ = nullptr;
        timed_out_ = false;
        resume_posted_ = false;
        return result;
    }

private:
    static void on_notify(BodyReadAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    static void on_timeout(BodyReadAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->timed_out_ = true;
        if (awaiter->request_ && awaiter->request_->body_waiter_ == awaiter) {
            awaiter->request_->body_waiter_ = nullptr;
        }
        awaiter->resume();
    }

    void resume() noexcept {
        auto handle = handle_;
        handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    bool has_timer() const noexcept {
        return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
    }

    ServerHttp2Request *request_ = nullptr;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    bool timed_out_ = false;
    bool resume_posted_ = false;

    friend class ServerHttp2Request;
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
        &ServerHttp2Request::on_indexed_field,
        &ServerHttp2Request::on_indexed_name,
        &ServerHttp2Request::on_name_raw,
        &ServerHttp2Request::on_name_huffman,
        &ServerHttp2Request::on_value_raw,
        &ServerHttp2Request::on_value_huffman,
    };
    return kOps;
}

const HeaderMap<ServerHttp2Request::PseudoHeaderHandler> &ServerHttp2Request::pseudo_header_handler_map() noexcept {
    static HeaderMap<PseudoHeaderHandler> handlers = []() {
        HeaderMap<PseudoHeaderHandler> map;
        map.insert(":method", &ServerHttp2Request::handle_method);
        map.insert(":path", &ServerHttp2Request::handle_path);
        map.insert(":scheme", &ServerHttp2Request::handle_scheme);
        map.insert(":authority", &ServerHttp2Request::handle_authority);
        return map;
    }();
    return handlers;
}

ServerHttp2Request::ServerHttp2Request(std::uint32_t stream_id, Http2Connection &conn,
                                       const HttpServerOptions &http_options,
                                       const HttpHandler &handler) noexcept :
    conn_(&conn), handler_(&handler), stream_(stream_id, this, stream_ops()), exchange_(http_options),
    body_timeout_(http_options.body_timeout) {
    FIBER_ASSERT(handler_ != nullptr);
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

common::IoErr ServerHttp2Request::on_header_block_start(void *owner, Http2HpackDecoder::Sink &sink) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (request->reading_trailers_ || request->exchange_.request_trailers_complete_) {
        return common::IoErr::Invalid;
    }
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_owned_ = false;
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
        if (!request->handler_started_) {
            request->exchange_.set_io(request);
            request->handler_started_ = true;
            fiber::async::spawn([request, lease = request->stream_.lease()]() mutable {
                return run_handler_task(request, std::move(lease));
            });
        }
        if (end_stream) {
            request->exchange_.request_trailers_complete_ = true;
            request->notify_body_waiter();
        }
        return common::IoErr::None;
    }

    if (!request->reading_trailers_ || !end_stream) {
        return common::IoErr::Invalid;
    }

    request->reading_trailers_ = true;
    request->exchange_.request_trailers_complete_ = true;
    request->notify_body_waiter();
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_body(void *owner, mem::IoBuf &&buf, bool end_stream) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (!request->request_head_received_ || request->reading_trailers_ || request->exchange_.request_trailers_complete_) {
        return common::IoErr::Invalid;
    }
    const bool queued_data = buf.readable() != 0;
    if (queued_data && !request->request_body_queue_.append(std::move(buf))) {
        return common::IoErr::NoMem;
    }
    if (end_stream) {
        request->exchange_.request_trailers_complete_ = true;
    }
    if (queued_data || end_stream) {
        request->notify_body_waiter();
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
    request->exchange_.set_io(nullptr);

    if (!request->stream_.local_end_stream() && !request->stream_.local_rst() && !request->stream_.remote_rst() &&
        request->stream_.close_reason() == common::IoErr::None) {
        (void) request->stream_.close_rst(Http2ErrorCode::Cancel, common::IoErr::NotSupported);
    }

    (void) lease;
    co_return;
}

common::IoErr ServerHttp2Request::encode_response_frames(Http2Stream &stream, void *ctx,
                                                         const Http2OutboundEncodeRequest &req,
                                                         Http2OutboundEncodeTarget &target,
                                                         Http2OutboundEncodeResult &result) noexcept {
    (void)stream;
    (void)req;

    auto *request = static_cast<ServerHttp2Request *>(ctx);
    if (!request) {
        return common::IoErr::Invalid;
    }
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() || request->stream_.remote_rst()) {
        request->pending_response_frames_.clear();
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }
    if (request->pending_response_frames_.readable_bytes() == 0) {
        result.status = Http2OutboundEncodeResult::Status::NoWork;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    common::IoErr err = target.append_chain(std::move(request->pending_response_frames_));
    if (err != common::IoErr::None) {
        return err;
    }

    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.next_kind = Http2OutboundNextKind::None;
    result.consumed_conn_window = 0;
    return common::IoErr::None;
}

ServerHttp2Request::BodyReadPollResult ServerHttp2Request::poll_body_read_state() const noexcept {
    BodyReadPollResult result{};
    if (request_body_queue_.readable_bytes() != 0) {
        result.kind = BodyReadPollResult::Kind::Readable;
        return result;
    }
    if (exchange_.request_trailers_complete_) {
        result.kind = BodyReadPollResult::Kind::End;
        return result;
    }
    if (abort_reason_ != common::IoErr::None) {
        result.kind = BodyReadPollResult::Kind::Closed;
        result.error = abort_reason_;
        return result;
    }
    return result;
}

bool ServerHttp2Request::arm_body_waiter(BodyReadAwaiter *awaiter) noexcept {
    if (!awaiter || poll_body_read_state().kind != BodyReadPollResult::Kind::Wait) {
        return false;
    }
    FIBER_ASSERT(body_waiter_ == nullptr);
    body_waiter_ = awaiter;
    return true;
}

void ServerHttp2Request::cancel_body_waiter(BodyReadAwaiter *awaiter) noexcept {
    if (body_waiter_ == awaiter) {
        body_waiter_ = nullptr;
    }
}

void ServerHttp2Request::notify_body_waiter() noexcept {
    if (!body_waiter_ || body_waiter_->resume_posted_ || body_waiter_->loop_ == nullptr) {
        return;
    }
    body_waiter_->resume_posted_ = true;
    body_waiter_->loop_->post<BodyReadAwaiter, &BodyReadAwaiter::notify_entry_, &BodyReadAwaiter::on_notify>(*body_waiter_);
}

void ServerHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    pending_response_frames_.clear();
    notify_body_waiter();
}

fiber::async::Task<common::IoResult<BodyChunk>> ServerHttp2Request::read_body(HttpExchange &, std::size_t max_bytes) noexcept {
    BodyChunk out{};
    if (max_bytes == 0) {
        if (request_body_queue_.readable_bytes() == 0 && exchange_.request_trailers_complete_) {
            out.last = true;
        }
        co_return out;
    }

    BodyReadPollResult state = co_await BodyReadAwaiter(*this, body_timeout_);
    switch (state.kind) {
        case BodyReadPollResult::Kind::Readable:
            break;
        case BodyReadPollResult::Kind::End:
            out.last = true;
            co_return out;
        case BodyReadPollResult::Kind::TimedOut:
            co_return std::unexpected(common::IoErr::TimedOut);
        case BodyReadPollResult::Kind::Closed:
            co_return std::unexpected(state.error);
        case BodyReadPollResult::Kind::Wait:
            FIBER_ASSERT(false);
            co_return std::unexpected(common::IoErr::Invalid);
    }

    const std::size_t queued_bytes = request_body_queue_.readable_bytes();
    const std::size_t take = std::min(max_bytes, queued_bytes);
    if (!request_body_queue_.take_prefix(take, out.data_chain)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    if (common::IoErr err = stream_.maybe_replenish_recv_window(request_body_queue_.readable_bytes());
        err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    if (request_body_queue_.readable_bytes() == 0 && exchange_.request_trailers_complete_) {
        out.last = true;
    }
    co_return out;
}

common::IoErr ServerHttp2Request::prepare_final_header(const OutgoingHeaderBlockView &header) noexcept {
    if (response_headers_sent_ || response_finished_) {
        return common::IoErr::Already;
    }
    if (header.status_code < 200 || header.status_code > 999) {
        return common::IoErr::Invalid;
    }
    if (header.kind != OutgoingHeaderKind::Final) {
        return common::IoErr::Invalid;
    }
    if (header.connection_mode == ResponseConnectionMode::Close) {
        return common::IoErr::NotSupported;
    }
    if (!header.end_stream) {
        return common::IoErr::NotSupported;
    }
    if (header.body_mode == ResponseBodyMode::Chunked) {
        return common::IoErr::NotSupported;
    }
    if (header.body_mode == ResponseBodyMode::ContentLength && header.content_length != 0) {
        return common::IoErr::NotSupported;
    }

    response_status_code_ = header.status_code;
    response_reason_ = header.reason;
    response_headers_ = header.headers;
    response_body_mode_ = header.body_mode;
    response_connection_mode_ = header.connection_mode;
    response_content_length_ = header.content_length;
    return common::IoErr::None;
}

fiber::async::Task<common::IoResult<void>> ServerHttp2Request::send_response_header_block(const HttpHeaders *headers,
                                                                                           int status_code,
                                                                                           bool end_stream,
                                                                                           bool informational) {
    if (conn_ == nullptr || !handler_started_ || stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (!informational && (response_headers_sent_ || response_finished_)) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (status_code < 100 || status_code > 999) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (informational && (status_code < 100 || status_code >= 200)) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!informational && status_code >= 100 && status_code < 200) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!informational && response_body_mode_ == ResponseBodyMode::Chunked) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }
    if (!informational && response_body_mode_ == ResponseBodyMode::ContentLength && response_content_length_ != 0) {
        co_return std::unexpected(common::IoErr::NotSupported);
    }

    Http2HeadersFrameEncoder frame_encoder(conn_->outbound_hpack_encoder(), {
        .stream_id = stream_.stream_id(),
        .max_frame_size = conn_->peer_max_outbound_frame_size(),
        .first_frame_payload_cap = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(conn_->peer_max_outbound_frame_size(), 1024)),
        .end_stream = end_stream,
    });
    common::IoErr err = frame_encoder.begin();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    err = frame_encoder.encode_status(status_code);
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    bool has_content_length_header = false;
    if (headers != nullptr) {
        for (auto it = headers->begin(); it != headers->end(); ++it) {
            const auto &field = *it;
            if (field.name_len == 0) {
                continue;
            }
            std::string_view lowcase_name = field.lowcase_view();
            if (lowcase_name.empty()) {
                lowcase_name = field.name_view();
            }
            if (is_forbidden_http2_response_header(lowcase_name)) {
                continue;
            }
            if (!informational && lowcase_name == kContentLengthHeader) {
                has_content_length_header = true;
            }

            err = frame_encoder.encode_field(lowcase_name, field.name_hash, field.value_view());
            if (err != common::IoErr::None) {
                co_return std::unexpected(err);
            }
        }
    }

    if (!informational && response_body_mode_ == ResponseBodyMode::ContentLength && !has_content_length_header) {
        std::array<char, 20> content_length_buf{};
        std::string_view content_length = format_content_length(response_content_length_, content_length_buf);
        err = frame_encoder.encode_field(kContentLengthHeader, http_header_name_hash(kContentLengthHeader),
                                         content_length);
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
    }

    mem::IoBufChain frames;
    err = frame_encoder.finish(frames);
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    if (!frames.take_prefix(frames.readable_bytes(), pending_response_frames_)) {
        co_return std::unexpected(common::IoErr::NoMem);
    }

    err = conn_->request_stream_send(stream_, Http2OutboundNextKind::Headers, &ServerHttp2Request::encode_response_frames,
                                     this);
    if (err != common::IoErr::None) {
        pending_response_frames_.clear();
        co_return std::unexpected(err);
    }

    if (end_stream) {
        stream_.local_end_stream_ = true;
        conn_->try_release_stream(stream_);
    }

    if (!informational) {
        response_headers_sent_ = true;
        response_finished_ = end_stream;
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> ServerHttp2Request::send_header(HttpExchange &exchange,
                                                                            const OutgoingHeaderBlockView &header) {
    if (conn_ == nullptr || &exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    switch (header.kind) {
        case OutgoingHeaderKind::Informational:
            co_return co_await send_response_header_block(header.headers, header.status_code, false, true);
        case OutgoingHeaderKind::Final:
            {
                common::IoErr err = prepare_final_header(header);
                if (err != common::IoErr::None) {
                    co_return std::unexpected(err);
                }
                co_return co_await send_response_header_block(header.headers, header.status_code, true, false);
            }
        case OutgoingHeaderKind::Trailer:
            co_return std::unexpected(common::IoErr::NotSupported);
    }

    co_return std::unexpected(common::IoErr::Invalid);
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write_body(HttpExchange &, BodyChunk) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write_body(HttpExchange &, const std::uint8_t *,
                                                                            std::size_t, bool) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

common::IoErr ServerHttp2Request::on_indexed_field(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    return request->commit_field(entry.name, entry.name_hash, entry.value);
}

common::IoErr ServerHttp2Request::on_indexed_name(void *owner, std::string_view name,
                                                  std::uint64_t name_hash) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    request->pending_name_ = name;
    request->pending_name_hash_ = name_hash;
    request->pending_name_owned_ = false;
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
    request->pending_name_owned_ = true;
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
    request->pending_name_owned_ = true;
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
    bool pending_name_owned = request->pending_name_owned_;
    if (out != nullptr && !pending_name_owned) {
        pending_name = request->copy_to_pool(pending_name);
        if (!pending_name.data() && !request->pending_name_.empty()) {
            return common::IoErr::NoMem;
        }
        pending_name_owned = true;
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
    request->pending_name_owned_ = false;
    return request->commit_field(pending_name, pending_name_hash, value, pending_name_owned);
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
    bool pending_name_owned = request->pending_name_owned_;
    if (out != nullptr && !pending_name_owned) {
        pending_name = request->copy_to_pool(pending_name);
        if (!pending_name.data() && !request->pending_name_.empty()) {
            return common::IoErr::NoMem;
        }
        pending_name_owned = true;
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
    request->pending_name_owned_ = false;
    return request->commit_field(pending_name, pending_name_hash, value, pending_name_owned);
}

common::IoErr ServerHttp2Request::materialize_name_raw(const std::uint8_t *data, std::size_t len,
                                                       std::string_view &out, std::uint64_t &name_hash) noexcept {
    std::string_view name = copy_to_pool(data, len);
    if (!name.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    out = name;
    name_hash = http_header_name_hash(name);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                           std::string_view &out,
                                                           std::uint64_t &name_hash) noexcept {
    out = {};
    bool ok = false;
    std::size_t decoded_len = http2_huffman_decoded_length(data, len, &ok);
    if (!ok) {
        return common::IoErr::Invalid;
    }
    if (decoded_len != 0) {
        auto *mem = static_cast<char *>(exchange_.pool_.alloc(decoded_len));
        if (!mem) {
            return common::IoErr::NoMem;
        }

        Http2HuffmanDecodeState state;
        Http2HuffmanDecodeResult result =
            http2_huffman_decode_exact(state, data, len, reinterpret_cast<std::uint8_t *>(mem), true);
        if (result.code != Http2HuffmanCode::Ok || result.written != decoded_len) {
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
    std::size_t decoded_len = http2_huffman_decoded_length(data, len, &ok);
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

    Http2HuffmanDecodeState state;
    Http2HuffmanDecodeResult result =
        http2_huffman_decode_exact(state, data, len, reinterpret_cast<std::uint8_t *>(mem), true);
    if (result.code != Http2HuffmanCode::Ok || result.written != decoded_len) {
        return common::IoErr::Invalid;
    }

    out = std::string_view(mem, decoded_len);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                               bool name_owned) noexcept {
    if (is_pseudo_header(name)) {
        if (reading_trailers_ || saw_regular_header_in_block_) {
            return common::IoErr::Invalid;
        }

        auto *handler = pseudo_header_handler_map().get(name);
        if (!handler) {
            return common::IoErr::Invalid;
        }
        return (*handler)(*this, value);
    }

    saw_regular_header_in_block_ = true;
    return commit_regular_header(name, name_hash, value, name_owned);
}

common::IoErr ServerHttp2Request::commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                        std::string_view value, bool name_owned) noexcept {
    std::string_view name_copy = name_owned ? name : copy_to_pool(name);
    std::string_view value_copy = copy_to_pool(value);
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

common::IoErr ServerHttp2Request::handle_method(ServerHttp2Request &request, std::string_view value) noexcept {
    std::string_view method = request.copy_to_pool(value);
    if (method.data() == nullptr && !method.empty()) {
        return common::IoErr::NoMem;
    }
    request.exchange_.method_view_ = method;
    request.exchange_.method_ = parse_method(method);
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_path(ServerHttp2Request &request, std::string_view value) noexcept {
    std::string_view path = request.copy_to_pool(value);
    if (path.data() == nullptr && !path.empty()) {
        return common::IoErr::NoMem;
    }
    request.exchange_.uri_.unparsed_uri = path;
    request.exchange_.uri_.path = path;
    request.exchange_.uri_.query = {};
    request.exchange_.uri_.exten = {};
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '?') {
            request.exchange_.uri_.path = path.substr(0, i);
            request.exchange_.uri_.query = path.substr(i + 1);
            break;
        }
    }
    std::size_t slash = request.exchange_.uri_.path.find_last_of('/');
    std::size_t dot = request.exchange_.uri_.path.find_last_of('.');
    if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash + 1)) {
        request.exchange_.uri_.exten = request.exchange_.uri_.path.substr(dot + 1);
    }
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_scheme(ServerHttp2Request &request, std::string_view) noexcept {
    request.exchange_.version_ = HttpVersion::HTTP_2_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::handle_authority(ServerHttp2Request &request, std::string_view value) noexcept {
    return request.commit_regular_header("host", http_header_name_hash("host"), value);
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
