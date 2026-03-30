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

void noop_abort(void *, common::IoErr) noexcept {}

} // namespace

class ClientHttp2Request::HeaderSendAwaiter {
public:
    HeaderSendAwaiter(ClientHttp2Request &request, const Http2RequestHead &head, bool end_stream) noexcept :
        request_(&request),
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

    ~HeaderSendAwaiter() { on_destroy_cleanup(); }

    bool await_ready() const noexcept { return completed_; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (completed_) {
            return false;
        }
        loop_ = &fiber::event::EventLoop::current();
        handle_ = handle;
        return true;
    }

    common::IoResult<void> await_resume() noexcept {
        common::IoErr result = take_result();
        if (result != common::IoErr::None) {
            return std::unexpected(result);
        }
        return common::IoResult<void>{};
    }

    void on_abort(common::IoErr result) noexcept {
        if (request_) {
            (void) request_->cancel_queued_send();
        }
        complete(result);
    }

private:
    static void on_notify(HeaderSendAwaiter *awaiter) {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        awaiter->resume();
    }

    void on_destroy_cleanup() noexcept {
        if (!request_) {
            return;
        }
        (void) request_->cancel_queued_send();
        if (request_->send_awaiter_ == this) {
            request_->send_awaiter_ = nullptr;
        }
        request_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        completed_ = false;
        result_ = common::IoErr::None;
        resume_posted_ = false;
    }

    common::IoErr take_result() noexcept {
        common::IoErr result = result_;
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
        loop_->post<HeaderSendAwaiter, &HeaderSendAwaiter::notify_entry_, &HeaderSendAwaiter::on_notify>(*this);
    }

    void resume() noexcept {
        auto handle = handle_;
        handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    ClientHttp2Request *request_ = nullptr;
    HttpMethod method_ = HttpMethod::Unknown;
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view path_{};
    const HttpHeaders *headers_ = nullptr;
    bool end_stream_ = false;
    fiber::event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    fiber::event::EventLoop::NotifyEntry notify_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool completed_ = false;
    bool resume_posted_ = false;

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

    HeaderSendAwaiter awaiter(*this, head, end_stream);
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
        request->conn_->try_release_stream(request->stream_);
    }

    request->on_header_send_complete(awaiter, common::IoErr::None);
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

bool ClientHttp2Request::cancel_queued_send() noexcept {
    return conn_ != nullptr && conn_->cancel_queued_stream_send(stream_);
}

void ClientHttp2Request::on_header_send_complete(HeaderSendAwaiter *awaiter, common::IoErr result) noexcept {
    if (awaiter) {
        awaiter->complete(result);
    }
}

void ClientHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    abort_reason_ = reason;
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_abort(reason);
    }
}

void ClientHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Request *>(owner); }

} // namespace fiber::http
