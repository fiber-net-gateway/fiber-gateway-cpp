#include "ServerHttp2Request.h"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <limits>
#include <new>

#include "../common/Assert.h"
#include "../common/IoError.h"
#include "../event/EventLoop.h"
#include "Http2Connection.h"
#include "Http2DataFrameEncoder.h"
#include "Http2HeadersFrameEncoder.h"
#include "Http2HpackHuffman.h"

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

bool is_pseudo_header(std::string_view name) noexcept {
    return !name.empty() && name.front() == ':';
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

struct ServerHttp2Request::SendResponseHeaderOp {
    using SuccessType = void;

    SendResponseHeaderOp(const OutgoingHeaderBlockView &header) noexcept :
        kind_(header.kind),
        headers_(header.headers),
        status_code_(header.status_code),
        reason_(header.reason),
        end_stream_(header.end_stream),
        informational_(header.kind == OutgoingHeaderKind::Informational) {}

    [[nodiscard]] common::IoErr submit(ServerHttp2Request &request, HeaderSendAwaiter &awaiter) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundNextKind::Headers,
                                                  &ServerHttp2Request::encode_response_frames, &awaiter);
    }

    OutgoingHeaderKind kind_ = OutgoingHeaderKind::Final;
    const HttpHeaders *headers_ = nullptr;
    int status_code_ = 0;
    std::string_view reason_;
    bool end_stream_ = false;
    bool informational_ = false;
};

struct ServerHttp2Request::SendResponseBodyOp {
    using SuccessType = std::size_t;

    explicit SendResponseBodyOp(BodyChunk &&chunk) noexcept :
        chunk_(std::move(chunk)), total_bytes_(chunk_.data_chain.readable_bytes()) {}

    [[nodiscard]] bool should_complete_without_submit() const noexcept {
        return chunk_.data_chain.readable_bytes() == 0 && !chunk_.last;
    }

    [[nodiscard]] bool needs_stream_window() const noexcept { return chunk_.data_chain.readable_bytes() != 0; }

    [[nodiscard]] common::IoErr submit(ServerHttp2Request &request, BodySendAwaiter &awaiter) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundNextKind::Data,
                                                  &ServerHttp2Request::encode_body_frames, &awaiter);
    }

    [[nodiscard]] std::size_t success_result(const BodySendAwaiter &) const noexcept { return total_bytes_; }

    BodyChunk chunk_{};
    std::size_t total_bytes_ = 0;
};

const Http2Stream::Ops &ServerHttp2Request::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
        &ServerHttp2Request::destroy_owner,
        &ServerHttp2Request::on_header_block_start,
        &ServerHttp2Request::on_header_block_complete,
        &ServerHttp2Request::on_body,
        &ServerHttp2Request::on_stream_abort,
        &ServerHttp2Request::on_stream_send_window_available,
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
    conn_(&conn), handler_(&handler), stream_(this, stream_ops()), exchange_(http_options),
    body_timeout_(http_options.body_timeout), write_timeout_(http_options.write_timeout) {
    (void) stream_id;
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

void ServerHttp2Request::on_stream_send_window_available(void *owner) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    request->on_stream_send_window_available();
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
    auto *awaiter = static_cast<HeaderSendAwaiter *>(ctx);
    if (!awaiter || !awaiter->owner()) {
        return common::IoErr::Invalid;
    }
    auto *request = awaiter->owner();
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() || request->stream_.remote_rst()) {
        request->on_header_send_complete(awaiter, request->abort_reason_ != common::IoErr::None
                                                      ? request->abort_reason_
                                                      : common::IoErr::Canceled);
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    Http2HeadersFrameEncoder frame_encoder(request->conn_->outbound_hpack_encoder(), {
        .stream_id = stream.stream_id(),
        .max_frame_size = req.max_frame_size,
        .first_frame_payload_cap = static_cast<std::uint16_t>(std::min<std::uint32_t>(
            req.max_frame_size,
            static_cast<std::uint32_t>(std::min<std::size_t>(
                target.slot_available() > 9 ? target.slot_available() - 9 : 0,
                static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()))))),
        .end_stream = awaiter->op_.end_stream_,
    });
    common::IoErr err = frame_encoder.begin(target);
    if (err != common::IoErr::None) {
        request->on_header_send_complete(awaiter, err);
        return err;
    }

    if (awaiter->op_.kind_ != OutgoingHeaderKind::Trailer) {
        err = frame_encoder.encode_status(awaiter->op_.status_code_);
        if (err != common::IoErr::None) {
            frame_encoder.abort();
            request->on_header_send_complete(awaiter, err);
            return err;
        }
    }

    if (awaiter->op_.headers_ != nullptr) {
        for (auto it = awaiter->op_.headers_->begin(); it != awaiter->op_.headers_->end(); ++it) {
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
                request->on_header_send_complete(awaiter, err);
                return err;
            }
        }
    }

    err = frame_encoder.finish();
    if (err != common::IoErr::None) {
        request->on_header_send_complete(awaiter, err);
        return err;
    }

    if (awaiter->op_.end_stream_) {
        request->stream_.local_end_stream_ = true;
        request->response_finished_ = true;
        request->conn_->try_release_stream(request->stream_);
    }

    if (awaiter->op_.kind_ == OutgoingHeaderKind::Final) {
        request->response_headers_sent_ = true;
        request->response_finished_ = awaiter->op_.end_stream_;
        request->response_status_code_ = awaiter->op_.status_code_;
        request->response_reason_ = awaiter->op_.reason_;
        request->response_headers_ = awaiter->op_.headers_;
    }

    request->on_header_send_complete(awaiter, common::IoErr::None);
    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.next_kind = Http2OutboundNextKind::None;
    result.consumed_conn_window = 0;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::encode_body_frames(Http2Stream &stream, void *ctx,
                                                     const Http2OutboundEncodeRequest &req,
                                                     Http2OutboundEncodeTarget &target,
                                                     Http2OutboundEncodeResult &result) noexcept {
    auto *awaiter = static_cast<BodySendAwaiter *>(ctx);
    if (!awaiter || !awaiter->owner()) {
        return common::IoErr::Invalid;
    }

    auto *request = awaiter->owner();
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() || request->stream_.remote_rst()) {
        request->on_body_send_complete(awaiter, request->abort_reason_ != common::IoErr::None ? request->abort_reason_
                                                                                               : common::IoErr::Canceled);
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    awaiter->clear_waiting_stream_window();

    const std::size_t remaining = awaiter->op_.chunk_.data_chain.readable_bytes();
    const std::int32_t stream_window = stream.send_window();
    const std::uint32_t stream_budget = stream_window > 0 ? static_cast<std::uint32_t>(stream_window) : 0U;
    const std::uint32_t conn_budget = req.conn_window_budget > 0 ? static_cast<std::uint32_t>(req.conn_window_budget) : 0U;

    if (remaining == 0) {
        if (!awaiter->op_.chunk_.last) {
            request->on_body_send_complete(awaiter, common::IoErr::None);
            result.status = Http2OutboundEncodeResult::Status::NoWork;
            result.next_kind = Http2OutboundNextKind::None;
            return common::IoErr::None;
        }

        Http2DataFrameEncoder frame_encoder({
            .stream_id = stream.stream_id(),
            .max_frame_size = req.max_frame_size,
            .end_stream = true,
        });
        common::IoErr err = frame_encoder.encode(target, awaiter->op_.chunk_.data_chain, 0);
        if (err != common::IoErr::None) {
            request->on_body_send_complete(awaiter, err);
            return err;
        }

        request->stream_.local_end_stream_ = true;
        request->response_finished_ = true;
        request->conn_->try_release_stream(request->stream_);
        request->on_body_send_complete(awaiter, common::IoErr::None);
        result.status = Http2OutboundEncodeResult::Status::Encoded;
        result.next_kind = Http2OutboundNextKind::None;
        result.consumed_conn_window = 0;
        return common::IoErr::None;
    }

    if (stream_budget == 0) {
        awaiter->mark_waiting_stream_window();
        result.status = Http2OutboundEncodeResult::Status::NoWork;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    if (conn_budget == 0) {
        result.status = Http2OutboundEncodeResult::Status::BlockedConnWindow;
        result.next_kind = Http2OutboundNextKind::Data;
        return common::IoErr::None;
    }

    const std::size_t payload_budget =
        std::min<std::size_t>(remaining, std::min<std::uint32_t>(conn_budget, stream_budget));
    Http2DataFrameEncoder frame_encoder({
        .stream_id = stream.stream_id(),
        .max_frame_size = req.max_frame_size,
        .end_stream = awaiter->op_.chunk_.last && payload_budget == remaining,
    });
    common::IoErr err = frame_encoder.encode(target, awaiter->op_.chunk_.data_chain, payload_budget);
    if (err != common::IoErr::None) {
        request->on_body_send_complete(awaiter, err);
        return err;
    }

    request->response_body_sent_ += payload_budget;
    const std::size_t after_remaining = awaiter->op_.chunk_.data_chain.readable_bytes();
    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.consumed_conn_window = static_cast<std::uint32_t>(payload_budget);

    if (after_remaining == 0) {
        if (awaiter->op_.chunk_.last) {
            request->stream_.local_end_stream_ = true;
            request->response_finished_ = true;
            request->conn_->try_release_stream(request->stream_);
        }
        result.next_kind = Http2OutboundNextKind::None;
        request->on_body_send_complete(awaiter, common::IoErr::None);
        return common::IoErr::None;
    }

    if (payload_budget == conn_budget) {
        result.next_kind = Http2OutboundNextKind::Data;
        return common::IoErr::None;
    }

    awaiter->mark_waiting_stream_window();
    result.next_kind = Http2OutboundNextKind::None;
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

bool ServerHttp2Request::cancel_queued_send() noexcept {
    if (conn_ == nullptr) {
        return false;
    }
    return conn_->cancel_queued_stream_send(stream_);
}

void ServerHttp2Request::on_stream_send_window_available() noexcept {
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_stream_send_window_available();
    }
}

void ServerHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_abort(abort_reason_);
    }
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

fiber::async::Task<common::IoResult<void>> ServerHttp2Request::send_header(HttpExchange &exchange,
                                                                            const OutgoingHeaderBlockView &header) {
    if (conn_ == nullptr || &exchange != &exchange_) {
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
    }

    HeaderSendAwaiter awaiter(*this, write_timeout_, header);
    if (!awaiter.try_arm()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    common::IoErr err = awaiter.start();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write_body(HttpExchange &exchange,
                                                                            BodyChunk chunk) noexcept {
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

    BodySendAwaiter awaiter(*this, write_timeout_, std::move(chunk));
    if (!awaiter.try_arm()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    common::IoErr err = awaiter.start();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }

    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<size_t>> ServerHttp2Request::write_body(HttpExchange &exchange,
                                                                            const std::uint8_t *buf, std::size_t len,
                                                                            bool end) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    BodyChunk chunk;
    chunk.last = end;
    if (len != 0) {
        mem::IoBuf owned = mem::IoBuf::allocate(len);
        if (!owned) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(owned.writable_data(), buf, len);
        owned.commit(len);
        if (!chunk.data_chain.append(std::move(owned))) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
    }

    co_return co_await write_body(exchange, std::move(chunk));
}

void ServerHttp2Request::on_send_complete(SendAwaiter *awaiter, common::IoErr result) noexcept {
    if (!awaiter || send_awaiter_ != awaiter) {
        return;
    }
    awaiter->complete(result);
}

void ServerHttp2Request::on_header_send_complete(HeaderSendAwaiter *awaiter, common::IoErr result) noexcept {
    on_send_complete(awaiter, result);
}

void ServerHttp2Request::on_body_send_complete(BodySendAwaiter *awaiter, common::IoErr result) noexcept {
    on_send_complete(awaiter, result);
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
