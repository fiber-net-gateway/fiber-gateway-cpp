#include "ClientHttp2Request.h"

#include <algorithm>
#include <coroutine>
#include <limits>
#include <new>

#include "../common/Assert.h"
#include "../event/EventLoop.h"
#include "ClientHttp2Exchange.h"
#include "ClientHttp2Push.h"
#include "Http2Connection.h"
#include "Http2DataFrameEncoder.h"
#include "Http2HeadersFrameEncoder.h"

namespace fiber::http {

namespace {

common::IoErr noop_indexed_field(void *, Http2HpackDecoder::TableEntryView) noexcept { return common::IoErr::None; }

common::IoErr noop_indexed_name(void *, std::string_view, std::uint64_t) noexcept { return common::IoErr::None; }

common::IoErr noop_name_raw(void *, const std::uint8_t *, std::size_t) noexcept { return common::IoErr::None; }

common::IoErr noop_name_huffman(void *, const std::uint8_t *, std::size_t) noexcept { return common::IoErr::None; }

common::IoErr noop_value_raw(void *, const std::uint8_t *, std::size_t, Http2HpackDecoder::FieldView *) noexcept {
    return common::IoErr::None;
}

common::IoErr noop_value_huffman(void *, const std::uint8_t *, std::size_t, Http2HpackDecoder::FieldView *) noexcept {
    return common::IoErr::None;
}

common::IoErr noop_header_block_start(void *, Http2HpackDecoder::Sink &sink) noexcept {
    static const Http2HpackDecoder::Ops kDecoderOps{
        &noop_indexed_field,
        &noop_indexed_name,
        &noop_name_raw,
        &noop_name_huffman,
        &noop_value_raw,
        &noop_value_huffman,
    };
    sink.ctx = nullptr;
    sink.ops = &kDecoderOps;
    return common::IoErr::None;
}

common::IoErr noop_header_block_complete(void *, bool) noexcept { return common::IoErr::None; }

common::IoErr noop_body(void *, mem::IoBuf &&, bool) noexcept { return common::IoErr::None; }

} // namespace

class ClientHttp2Request::SendAwaiter {
public:
    SendAwaiter(ClientHttp2Request &request, std::chrono::milliseconds timeout) noexcept :
        request_(&request), timeout_(timeout) {}

    SendAwaiter(const SendAwaiter &) = delete;
    SendAwaiter &operator=(const SendAwaiter &) = delete;
    SendAwaiter(SendAwaiter &&) = delete;
    SendAwaiter &operator=(SendAwaiter &&) = delete;

    virtual ~SendAwaiter() {
        if (!request_) {
            return;
        }
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<SendAwaiter, &SendAwaiter::timer_entry_>(*this);
        }
        if (request_->send_awaiter_ == this) {
            request_->send_awaiter_ = nullptr;
        }
        request_ = nullptr;
    }

    bool await_ready() noexcept {
        if (!request_ || completed_) {
            return true;
        }
        if (timeout_.count() == 0) {
            on_timeout_ready();
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (!request_ || completed_) {
            return false;
        }
        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        if (has_timer()) {
            loop_->post_at<SendAwaiter, &SendAwaiter::timer_entry_, &SendAwaiter::on_timeout>(loop_->now() + timeout_,
                                                                                               *this);
        }
        return true;
    }

private:
    static void on_notify(SendAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    static void on_timeout(SendAwaiter *awaiter) {
        if (!awaiter || awaiter->completed_) {
            return;
        }
        awaiter->on_timeout_fired();
    }

protected:
    common::IoErr take_result() noexcept {
        common::IoErr result = result_;
        if (loop_ && timer_entry_.is_in_heap()) {
            loop_->cancel<SendAwaiter, &SendAwaiter::timer_entry_>(*this);
        }
        if (request_ && request_->send_awaiter_ == this) {
            request_->send_awaiter_ = nullptr;
        }
        request_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        completed_ = false;
        result_ = common::IoErr::None;
        resume_posted_ = false;
        return result;
    }

    virtual void on_abort(common::IoErr result) noexcept { complete(result); }
    virtual void on_stream_send_window_available() noexcept {}

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        post_resume();
    }

    void post_resume() noexcept {
        if (resume_posted_ || !loop_) {
            return;
        }
        resume_posted_ = true;
        loop_->post<SendAwaiter, &SendAwaiter::notify_entry_, &SendAwaiter::on_notify>(*this);
    }

    void resume() noexcept {
        auto handle = handle_;
        handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    [[nodiscard]] bool has_timer() const noexcept {
        return timeout_.count() > 0 && timeout_ != std::chrono::milliseconds::max();
    }

    virtual void on_destroy_cleanup() noexcept {}
    virtual void on_timeout_ready() noexcept = 0;
    virtual void on_timeout_fired() noexcept = 0;

    ClientHttp2Request *request_ = nullptr;
    std::chrono::milliseconds timeout_{};
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    fiber::event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool completed_ = false;
    bool resume_posted_ = false;

    friend class ClientHttp2Request;
};

class ClientHttp2Request::HeaderSendAwaiter final : public SendAwaiter {
public:
    HeaderSendAwaiter(ClientHttp2Request &request, const Http2RequestHead &head, bool end_stream,
                      std::chrono::milliseconds timeout) noexcept :
        SendAwaiter(request, timeout),
        method_(head.method),
        scheme_(head.scheme),
        authority_(head.authority),
        path_(head.path),
        headers_(head.headers),
        end_stream_(end_stream) {}

    HeaderSendAwaiter(const HeaderSendAwaiter &) = delete;
    HeaderSendAwaiter &operator=(const HeaderSendAwaiter &) = delete;
    HeaderSendAwaiter(HeaderSendAwaiter &&) = delete;
    HeaderSendAwaiter &operator=(HeaderSendAwaiter &&) = delete;

    ~HeaderSendAwaiter() override { on_destroy_cleanup(); }

    common::IoResult<void> await_resume() noexcept {
        common::IoErr result = take_result();
        if (result != common::IoErr::None) {
            return std::unexpected(result);
        }
        return common::IoResult<void>{};
    }

private:
    void on_destroy_cleanup() noexcept override {
        if (request_) {
            (void) request_->cancel_queued_send();
        }
    }

    void on_timeout_ready() noexcept override {
        if (request_ && request_->cancel_queued_send()) {
            completed_ = true;
            result_ = common::IoErr::TimedOut;
        }
    }

    void on_timeout_fired() noexcept override {
        if (!request_ || !request_->cancel_queued_send()) {
            return;
        }
        complete(common::IoErr::TimedOut);
    }

    HttpMethod method_ = HttpMethod::Unknown;
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view path_{};
    const HttpHeaders *headers_ = nullptr;
    bool end_stream_ = false;

    friend class ClientHttp2Request;
};

class ClientHttp2Request::BodySendAwaiter final : public SendAwaiter {
public:
    BodySendAwaiter(ClientHttp2Request &request, BodyChunk &&chunk, std::chrono::milliseconds timeout) noexcept :
        SendAwaiter(request, timeout), chunk_(std::move(chunk)), total_bytes_(chunk_.data_chain.readable_bytes()) {}

    BodySendAwaiter(const BodySendAwaiter &) = delete;
    BodySendAwaiter &operator=(const BodySendAwaiter &) = delete;
    BodySendAwaiter(BodySendAwaiter &&) = delete;
    BodySendAwaiter &operator=(BodySendAwaiter &&) = delete;

    ~BodySendAwaiter() override { on_destroy_cleanup(); }

    [[nodiscard]] common::IoErr start() noexcept {
        if (!request_) {
            return common::IoErr::Invalid;
        }
        if (chunk_.data_chain.readable_bytes() == 0 && !chunk_.last) {
            complete(common::IoErr::None);
            return common::IoErr::None;
        }
        if (need_stream_window() && request_->stream_.send_window() <= 0) {
            waiting_stream_window_ = true;
            return common::IoErr::None;
        }
        return request_submit();
    }

    common::IoResult<std::size_t> await_resume() noexcept {
        common::IoErr result = take_result();
        if (result != common::IoErr::None) {
            return std::unexpected(result);
        }
        return total_bytes_;
    }

private:
    static void on_submit_notify(BodySendAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->resume_submit_posted_ = false;
        awaiter->try_submit_from_window_signal();
    }

    void on_abort(common::IoErr result) noexcept override {
        if (request_) {
            (void) request_->cancel_queued_send();
        }
        waiting_stream_window_ = false;
        complete(result);
    }

    void on_stream_send_window_available() noexcept override {
        if (completed_ || !request_ || !waiting_stream_window_) {
            return;
        }
        if (resume_submit_posted_ || loop_ == nullptr) {
            return;
        }
        resume_submit_posted_ = true;
        loop_->post<BodySendAwaiter, &BodySendAwaiter::submit_notify_entry_, &BodySendAwaiter::on_submit_notify>(*this);
    }

    void on_destroy_cleanup() noexcept override {
        if (request_) {
            (void) request_->cancel_queued_send();
        }
        waiting_stream_window_ = false;
        resume_submit_posted_ = false;
    }

    void on_timeout_ready() noexcept override {
        if (!request_) {
            return;
        }
        if (waiting_stream_window_) {
            complete(common::IoErr::TimedOut);
            return;
        }
        if (request_->cancel_queued_send()) {
            complete(common::IoErr::TimedOut);
        }
    }

    void on_timeout_fired() noexcept override {
        if (!request_) {
            return;
        }
        if (request_->cancel_queued_send()) {
            waiting_stream_window_ = false;
            complete(common::IoErr::TimedOut);
            return;
        }
        if (waiting_stream_window_) {
            complete(common::IoErr::TimedOut);
        }
    }

    [[nodiscard]] bool need_stream_window() const noexcept { return chunk_.data_chain.readable_bytes() != 0; }

    [[nodiscard]] common::IoErr request_submit() noexcept {
        if (!request_) {
            return common::IoErr::Invalid;
        }
        waiting_stream_window_ = false;
        return request_->conn_->request_stream_send(request_->stream_, Http2OutboundNextKind::Data,
                                                    &ClientHttp2Request::encode_body_frames, this);
    }

    void try_submit_from_window_signal() noexcept {
        if (completed_ || !request_ || !waiting_stream_window_) {
            return;
        }
        if (request_->stream_.send_window() <= 0) {
            return;
        }
        common::IoErr err = request_submit();
        if (err != common::IoErr::None) {
            complete(err);
        }
    }

    BodyChunk chunk_{};
    std::size_t total_bytes_ = 0;
    bool waiting_stream_window_ = false;
    bool resume_submit_posted_ = false;
    fiber::event::EventLoop::NotifyEntry submit_notify_entry_{};

    friend class ClientHttp2Request;
};

class ClientHttp2Request::TrailerSendAwaiter final : public SendAwaiter {
public:
    TrailerSendAwaiter(ClientHttp2Request &request, const HttpHeaders &headers, std::chrono::milliseconds timeout) noexcept :
        SendAwaiter(request, timeout), headers_(&headers) {}

    TrailerSendAwaiter(const TrailerSendAwaiter &) = delete;
    TrailerSendAwaiter &operator=(const TrailerSendAwaiter &) = delete;
    TrailerSendAwaiter(TrailerSendAwaiter &&) = delete;
    TrailerSendAwaiter &operator=(TrailerSendAwaiter &&) = delete;

    ~TrailerSendAwaiter() override { on_destroy_cleanup(); }

    common::IoResult<void> await_resume() noexcept {
        common::IoErr result = take_result();
        if (result != common::IoErr::None) {
            return std::unexpected(result);
        }
        return common::IoResult<void>{};
    }

private:
    void on_destroy_cleanup() noexcept override {
        if (request_) {
            (void) request_->cancel_queued_send();
        }
    }

    void on_timeout_ready() noexcept override {
        if (request_ && request_->cancel_queued_send()) {
            completed_ = true;
            result_ = common::IoErr::TimedOut;
        }
    }

    void on_timeout_fired() noexcept override {
        if (!request_ || !request_->cancel_queued_send()) {
            return;
        }
        complete(common::IoErr::TimedOut);
    }

    const HttpHeaders *headers_ = nullptr;

    friend class ClientHttp2Request;
};

const Http2StreamFactoryOps &ClientHttp2Request::factory_ops() noexcept {
    static const Http2StreamFactoryOps kOps{
        &ClientHttp2Request::create_peer_stream_op,
    };
    return kOps;
}

Http2Stream::Lease ClientHttp2Request::create_peer_stream(std::uint32_t stream_id, Http2Connection &conn) noexcept {
    (void) stream_id;
    return ClientHttp2Push::create(stream_id, conn);
}

Http2Stream::Lease ClientHttp2Request::create_peer_stream_op(void *, std::uint32_t stream_id,
                                                             Http2Connection &conn) noexcept {
    return create_peer_stream(stream_id, conn);
}

const Http2Stream::Ops &ClientHttp2Request::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
        &ClientHttp2Request::destroy_owner,
        &noop_header_block_start,
        &noop_header_block_complete,
        &noop_body,
        &ClientHttp2Request::on_stream_abort,
        &ClientHttp2Request::on_stream_send_window_available,
    };
    return kOps;
}

ClientHttp2Request::ClientHttp2Request(Http2Connection &conn) noexcept : conn_(&conn), stream_(this, stream_ops()), pool_() {}

ClientHttp2Request *ClientHttp2Request::create(Http2Connection &conn) noexcept {
    return new (std::nothrow) ClientHttp2Request(conn);
}

fiber::async::Task<common::IoResult<void>> ClientHttp2Request::send_request_header(const Http2RequestHead &head,
                                                                                    bool end_stream) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (send_awaiter_ != nullptr || request_headers_sent_ || stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    HeaderSendAwaiter awaiter(*this, head, end_stream, conn_->options_.write_timeout);
    send_awaiter_ = &awaiter;
    common::IoErr err =
        conn_->request_stream_send(stream_, Http2OutboundNextKind::Headers, &ClientHttp2Request::encode_request_frames,
                                   &awaiter);
    if (err != common::IoErr::None) {
        send_awaiter_ = nullptr;
        co_return std::unexpected(err);
    }
    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<std::size_t>> ClientHttp2Request::write_body(BodyChunk chunk) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (send_awaiter_ != nullptr || request_finished_ || stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    BodySendAwaiter awaiter(*this, std::move(chunk), conn_->options_.write_timeout);
    send_awaiter_ = &awaiter;
    common::IoErr err = awaiter.start();
    if (err != common::IoErr::None) {
        send_awaiter_ = nullptr;
        co_return std::unexpected(err);
    }
    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<void>> ClientHttp2Request::write_trailer(const HttpHeaders &headers) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (send_awaiter_ != nullptr || request_finished_ || stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    TrailerSendAwaiter awaiter(*this, headers, conn_->options_.write_timeout);
    send_awaiter_ = &awaiter;
    common::IoErr err = conn_->request_stream_send(stream_, Http2OutboundNextKind::Headers,
                                                   &ClientHttp2Request::encode_trailer_frames, &awaiter);
    if (err != common::IoErr::None) {
        send_awaiter_ = nullptr;
        co_return std::unexpected(err);
    }
    co_return co_await awaiter;
}

common::IoErr ClientHttp2Request::encode_request_frames(Http2Stream &stream, void *ctx,
                                                        const Http2OutboundEncodeRequest &req,
                                                        Http2OutboundEncodeTarget &target,
                                                        Http2OutboundEncodeResult &result) noexcept {
    auto *awaiter = static_cast<HeaderSendAwaiter *>(ctx);
    if (!awaiter || !awaiter->request_) {
        return common::IoErr::Invalid;
    }
    auto *request = awaiter->request_;
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
        .end_stream = awaiter->end_stream_,
    });
    common::IoErr err = frame_encoder.begin(target);
    if (err != common::IoErr::None) {
        request->on_header_send_complete(awaiter, err);
        return err;
    }

    err = frame_encoder.encode_method(awaiter->method_);
    if (err == common::IoErr::None && !awaiter->scheme_.empty()) {
        err = frame_encoder.encode_scheme(awaiter->scheme_);
    }
    if (err == common::IoErr::None && !awaiter->authority_.empty()) {
        err = frame_encoder.encode_authority(awaiter->authority_);
    }
    if (err == common::IoErr::None && !awaiter->path_.empty()) {
        err = frame_encoder.encode_path(awaiter->path_);
    }
    if (err != common::IoErr::None) {
        frame_encoder.abort();
        request->on_header_send_complete(awaiter, err);
        return err;
    }

    if (awaiter->headers_ != nullptr) {
        for (auto it = awaiter->headers_->begin(); it != awaiter->headers_->end(); ++it) {
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

    request->request_headers_sent_ = true;
    if (awaiter->end_stream_) {
        request->stream_.local_end_stream_ = true;
        request->request_finished_ = true;
        request->conn_->try_release_stream(request->stream_);
    }

    request->on_header_send_complete(awaiter, common::IoErr::None);
    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.next_kind = Http2OutboundNextKind::None;
    result.consumed_conn_window = 0;
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::encode_body_frames(Http2Stream &stream, void *ctx,
                                                     const Http2OutboundEncodeRequest &req,
                                                     Http2OutboundEncodeTarget &target,
                                                     Http2OutboundEncodeResult &result) noexcept {
    auto *awaiter = static_cast<BodySendAwaiter *>(ctx);
    if (!awaiter || !awaiter->request_) {
        return common::IoErr::Invalid;
    }

    auto *request = awaiter->request_;
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() || request->stream_.remote_rst()) {
        request->on_body_send_complete(awaiter, request->abort_reason_ != common::IoErr::None ? request->abort_reason_
                                                                                               : common::IoErr::Canceled);
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    awaiter->waiting_stream_window_ = false;

    const std::size_t remaining = awaiter->chunk_.data_chain.readable_bytes();
    const std::int32_t stream_window = stream.send_window();
    const std::uint32_t stream_budget = stream_window > 0 ? static_cast<std::uint32_t>(stream_window) : 0U;
    const std::uint32_t conn_budget = req.conn_window_budget > 0 ? static_cast<std::uint32_t>(req.conn_window_budget) : 0U;

    if (remaining == 0) {
        if (!awaiter->chunk_.last) {
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
        common::IoErr err = frame_encoder.encode(target, awaiter->chunk_.data_chain, 0);
        if (err != common::IoErr::None) {
            request->on_body_send_complete(awaiter, err);
            return err;
        }

        request->stream_.local_end_stream_ = true;
        request->request_finished_ = true;
        request->conn_->try_release_stream(request->stream_);
        request->on_body_send_complete(awaiter, common::IoErr::None);
        result.status = Http2OutboundEncodeResult::Status::Encoded;
        result.next_kind = Http2OutboundNextKind::None;
        result.consumed_conn_window = 0;
        return common::IoErr::None;
    }

    if (stream_budget == 0) {
        awaiter->waiting_stream_window_ = true;
        result.status = Http2OutboundEncodeResult::Status::NoWork;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    if (conn_budget == 0) {
        result.status = Http2OutboundEncodeResult::Status::BlockedConnWindow;
        result.next_kind = Http2OutboundNextKind::Data;
        return common::IoErr::None;
    }

    const std::size_t payload_budget = std::min<std::size_t>(remaining, std::min<std::uint32_t>(conn_budget, stream_budget));
    Http2DataFrameEncoder frame_encoder({
        .stream_id = stream.stream_id(),
        .max_frame_size = req.max_frame_size,
        .end_stream = awaiter->chunk_.last && payload_budget == remaining,
    });
    common::IoErr err = frame_encoder.encode(target, awaiter->chunk_.data_chain, payload_budget);
    if (err != common::IoErr::None) {
        request->on_body_send_complete(awaiter, err);
        return err;
    }

    const std::size_t after_remaining = awaiter->chunk_.data_chain.readable_bytes();
    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.consumed_conn_window = static_cast<std::uint32_t>(payload_budget);

    if (after_remaining == 0) {
        if (awaiter->chunk_.last) {
            request->stream_.local_end_stream_ = true;
            request->request_finished_ = true;
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

    awaiter->waiting_stream_window_ = true;
    result.next_kind = Http2OutboundNextKind::None;
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::encode_trailer_frames(Http2Stream &stream, void *ctx,
                                                        const Http2OutboundEncodeRequest &req,
                                                        Http2OutboundEncodeTarget &target,
                                                        Http2OutboundEncodeResult &result) noexcept {
    auto *awaiter = static_cast<TrailerSendAwaiter *>(ctx);
    if (!awaiter || !awaiter->request_) {
        return common::IoErr::Invalid;
    }

    auto *request = awaiter->request_;
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() || request->stream_.remote_rst()) {
        request->on_trailer_send_complete(awaiter, request->abort_reason_ != common::IoErr::None
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
        .end_stream = true,
    });
    common::IoErr err = frame_encoder.begin(target);
    if (err != common::IoErr::None) {
        request->on_trailer_send_complete(awaiter, err);
        return err;
    }

    if (awaiter->headers_ != nullptr) {
        for (auto it = awaiter->headers_->begin(); it != awaiter->headers_->end(); ++it) {
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
                request->on_trailer_send_complete(awaiter, err);
                return err;
            }
        }
    }

    err = frame_encoder.finish();
    if (err != common::IoErr::None) {
        request->on_trailer_send_complete(awaiter, err);
        return err;
    }

    request->stream_.local_end_stream_ = true;
    request->request_finished_ = true;
    request->conn_->try_release_stream(request->stream_);
    request->on_trailer_send_complete(awaiter, common::IoErr::None);
    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.next_kind = Http2OutboundNextKind::None;
    result.consumed_conn_window = 0;
    return common::IoErr::None;
}

void ClientHttp2Request::on_stream_abort(void *owner, common::IoErr reason) noexcept {
    if (!owner) {
        return;
    }
    static_cast<ClientHttp2Request *>(owner)->on_stream_aborted(reason);
}

void ClientHttp2Request::on_stream_send_window_available(void *owner) noexcept {
    if (!owner) {
        return;
    }
    static_cast<ClientHttp2Request *>(owner)->on_stream_send_window_available();
}

bool ClientHttp2Request::cancel_queued_send() noexcept {
    return conn_ != nullptr && conn_->cancel_queued_stream_send(stream_);
}

void ClientHttp2Request::on_header_send_complete(HeaderSendAwaiter *awaiter, common::IoErr result) noexcept {
    if (!awaiter || send_awaiter_ != awaiter) {
        return;
    }
    awaiter->complete(result);
}

void ClientHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_abort(abort_reason_);
    }
}

void ClientHttp2Request::on_body_send_complete(BodySendAwaiter *awaiter, common::IoErr result) noexcept {
    if (!awaiter || send_awaiter_ != awaiter) {
        return;
    }
    awaiter->complete(result);
}

void ClientHttp2Request::on_trailer_send_complete(TrailerSendAwaiter *awaiter, common::IoErr result) noexcept {
    if (!awaiter || send_awaiter_ != awaiter) {
        return;
    }
    awaiter->complete(result);
}

void ClientHttp2Request::on_stream_send_window_available() noexcept {
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_stream_send_window_available();
    }
}

void ClientHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Request *>(owner); }

} // namespace fiber::http
