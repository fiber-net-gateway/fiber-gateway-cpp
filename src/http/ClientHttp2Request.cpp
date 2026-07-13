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
#include "detail/Http2HeaderDecodeUtil.h"

namespace fiber::http {

namespace {

bool is_status_informational(int status_code) noexcept { return status_code >= 100 && status_code < 200; }

bool is_valid_status_code(int status_code) noexcept { return status_code >= 100 && status_code <= 999; }

} // namespace

struct ClientHttp2Request::SendRequestHeaderOp {
    using SuccessType = void;

    SendRequestHeaderOp(const Http2RequestHead &head, bool end_stream) noexcept :
        method_(head.method), scheme_(head.scheme), authority_(head.authority), path_(head.path),
        headers_(head.headers), end_stream_(end_stream) {}

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request, HeaderSendAwaiter &awaiter) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundNextKind::Headers,
                                                  &ClientHttp2Request::encode_request_frames, &awaiter);
    }

    HttpMethod method_ = HttpMethod::Unknown;
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view path_{};
    const HttpHeaders *headers_ = nullptr;
    bool end_stream_ = false;
};

struct ClientHttp2Request::SendRequestBodyOp {
    using SuccessType = std::size_t;

    explicit SendRequestBodyOp(mem::IoBufChain &&chunk) noexcept :
        chunk_(std::move(chunk)), total_bytes_(chunk_.readable_bytes()) {}

    [[nodiscard]] bool should_complete_without_submit() const noexcept {
        return chunk_.readable_bytes() == 0 && !chunk_.complete();
    }

    [[nodiscard]] bool needs_stream_window() const noexcept { return chunk_.readable_bytes() != 0; }

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request, BodySendAwaiter &awaiter) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundNextKind::Data,
                                                  &ClientHttp2Request::encode_body_frames, &awaiter);
    }

    [[nodiscard]] std::size_t success_result(const BodySendAwaiter &) const noexcept { return total_bytes_; }

    mem::IoBufChain chunk_;
    std::size_t total_bytes_ = 0;
};

struct ClientHttp2Request::SendRequestTrailerOp {
    using SuccessType = void;

    explicit SendRequestTrailerOp(const HttpHeaders &headers) noexcept : headers_(&headers) {}

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request, TrailerSendAwaiter &awaiter) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundNextKind::Headers,
                                                  &ClientHttp2Request::encode_trailer_frames, &awaiter);
    }

    const HttpHeaders *headers_ = nullptr;
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
            &ClientHttp2Request::on_header_block_start,
            &ClientHttp2Request::on_header_block_complete,
            &ClientHttp2Request::on_body,
            &ClientHttp2Request::on_stream_abort,
            &ClientHttp2Request::on_stream_send_window_available,
    };
    return kOps;
}

const Http2HpackDecoder::Ops &ClientHttp2Request::decoder_ops() noexcept {
    static const Http2HpackDecoder::Ops kOps{
            &ClientHttp2Request::on_indexed_field, &ClientHttp2Request::on_indexed_name,
            &ClientHttp2Request::on_name_raw,      &ClientHttp2Request::on_name_huffman,
            &ClientHttp2Request::on_value_raw,     &ClientHttp2Request::on_value_huffman,
    };
    return kOps;
}

ClientHttp2Request::ClientHttp2Request(Http2Connection &conn, mem::BufPool &pool) noexcept :
    conn_(&conn), stream_(this, stream_ops()), pool_(&pool),
    response_body_recv_(conn.transport().loop().io_buf_node_pool()), response_header_recv_(pool) {}

ClientHttp2Request *ClientHttp2Request::create(Http2Connection &conn, mem::BufPool &pool) noexcept {
    return new (std::nothrow) ClientHttp2Request(conn, pool);
}

mem::IoBufNodePool &ClientHttp2Request::node_pool() noexcept {
    FIBER_ASSERT(conn_ != nullptr);
    return conn_->transport().loop().io_buf_node_pool();
}

fiber::async::Task<common::IoResult<void>>
ClientHttp2Request::send_request_header(const Http2RequestHead &head, bool end_stream,
                                        std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_headers_sent_ || stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    HeaderSendAwaiter awaiter(*this, timeout, head, end_stream);
    if (!awaiter.try_arm()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    common::IoErr err = awaiter.start();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<std::size_t>>
ClientHttp2Request::write_body(mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_finished_ || stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    BodySendAwaiter awaiter(*this, timeout, std::move(chunk));
    if (!awaiter.try_arm()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    common::IoErr err = awaiter.start();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<void>>
ClientHttp2Request::write_trailer(const HttpHeaders &headers, std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!request_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (request_finished_ || stream_.local_end_stream()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (abort_reason_ != common::IoErr::None) {
        co_return std::unexpected(abort_reason_);
    }
    if (stream_.local_rst() || stream_.remote_rst()) {
        co_return std::unexpected(common::IoErr::Canceled);
    }

    TrailerSendAwaiter awaiter(*this, timeout, headers);
    if (!awaiter.try_arm()) {
        co_return std::unexpected(common::IoErr::Already);
    }
    common::IoErr err = awaiter.start();
    if (err != common::IoErr::None) {
        co_return std::unexpected(err);
    }
    co_return co_await awaiter;
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
ClientHttp2Request::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await response_body_recv_.read_body(stream_, max_bytes, timeout);
}

fiber::async::Task<common::IoResult<const Http2ResponseHead *>>
ClientHttp2Request::read_header(std::chrono::milliseconds timeout) noexcept {
    if (conn_ == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await response_header_recv_.read_header(timeout);
}

common::IoErr ClientHttp2Request::on_header_block_start(void *owner, Http2HpackDecoder::Sink &sink) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
    if (request->current_header_node_ != nullptr) {
        return common::IoErr::Invalid;
    }
    if (request->response_head_received_) {
        if (request->stream_.remote_end_stream() || request->stream_.remote_rst()) {
            return common::IoErr::Invalid;
        }
        request->reading_trailers_ = true;
    } else {
        request->reading_trailers_ = false;
    }
    request->current_header_node_ = request->response_header_recv_.allocate_node();
    if (!request->current_header_node_) {
        return common::IoErr::NoMem;
    }
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    request->current_block_has_status_ = false;
    request->saw_regular_header_in_block_ = false;
    sink.ctx = request;
    sink.ops = &decoder_ops();
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::on_header_block_complete(void *owner, bool end_stream) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
    auto *node = request->current_header_node_;
    if (!node) {
        return common::IoErr::Invalid;
    }

    if (request->reading_trailers_) {
        if (request->current_block_has_status_ || !end_stream) {
            return common::IoErr::Invalid;
        }
        node->head.kind = OutgoingHeaderKind::Trailer;
        node->head.end_stream = end_stream;
        common::IoErr err = request->response_header_recv_.push_header_block(node);
        request->current_header_node_ = nullptr;
        request->pending_name_ = {};
        request->pending_name_hash_ = 0;
        request->pending_name_stable_ = false;
        if (err != common::IoErr::None) {
            return err;
        }
        if (end_stream) {
            request->response_body_recv_.close_input();
            request->response_header_recv_.close_input();
        }
        return common::IoErr::None;
    }

    if (!request->current_block_has_status_ || !is_valid_status_code(node->head.status_code)) {
        return common::IoErr::Invalid;
    }
    node->head.kind = is_status_informational(node->head.status_code) ? OutgoingHeaderKind::Informational
                                                                      : OutgoingHeaderKind::Final;
    if (end_stream && node->head.kind == OutgoingHeaderKind::Informational) {
        return common::IoErr::Invalid;
    }
    node->head.end_stream = end_stream;
    common::IoErr err = request->response_header_recv_.push_header_block(node);
    request->current_header_node_ = nullptr;
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    request->pending_name_stable_ = false;
    if (err != common::IoErr::None) {
        return err;
    }
    if (node->head.kind == OutgoingHeaderKind::Final) {
        request->response_head_received_ = true;
    }
    if (end_stream) {
        request->response_body_recv_.close_input();
        request->response_header_recv_.close_input();
    }
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::on_body(void *owner, mem::IoBuf &&buf, bool end_stream) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
    if (!request->response_head_received_ || request->reading_trailers_) {
        return common::IoErr::Invalid;
    }
    common::IoErr err = request->response_body_recv_.push_body(std::move(buf), end_stream);
    if (err != common::IoErr::None) {
        return err;
    }
    if (end_stream) {
        request->response_header_recv_.close_input();
    }
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::on_indexed_field(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
    const bool stable = entry.stable_for_exchange();
    return request->commit_field(entry.name, entry.name_hash, entry.value, stable, stable);
}

common::IoErr ClientHttp2Request::on_indexed_name(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
    request->pending_name_ = entry.name;
    request->pending_name_hash_ = entry.name_hash;
    request->pending_name_stable_ = entry.stable_for_exchange();
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
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

common::IoErr ClientHttp2Request::on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
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

common::IoErr ClientHttp2Request::on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                               Http2HpackDecoder::FieldView *out) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
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

common::IoErr ClientHttp2Request::on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                   Http2HpackDecoder::FieldView *out) noexcept {
    if (!owner) {
        return common::IoErr::Invalid;
    }
    auto *request = static_cast<ClientHttp2Request *>(owner);
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

common::IoErr ClientHttp2Request::encode_request_frames(Http2Stream &stream, void *ctx,
                                                        const Http2OutboundEncodeRequest &req,
                                                        Http2OutboundEncodeTarget &target,
                                                        Http2OutboundEncodeResult &result) noexcept {
    auto *awaiter = static_cast<HeaderSendAwaiter *>(ctx);
    if (!awaiter || !awaiter->owner()) {
        return common::IoErr::Invalid;
    }
    auto *request = awaiter->owner();
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() ||
        request->stream_.remote_rst()) {
        request->on_header_send_complete(awaiter, request->abort_reason_ != common::IoErr::None
                                                          ? request->abort_reason_
                                                          : common::IoErr::Canceled);
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    Http2HeadersFrameEncoder frame_encoder(
            request->conn_->outbound_hpack_encoder(),
            {
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

    err = frame_encoder.encode_method(awaiter->op_.method_);
    if (err == common::IoErr::None && !awaiter->op_.scheme_.empty()) {
        err = frame_encoder.encode_scheme(awaiter->op_.scheme_);
    }
    if (err == common::IoErr::None && !awaiter->op_.authority_.empty()) {
        err = frame_encoder.encode_authority(awaiter->op_.authority_);
    }
    if (err == common::IoErr::None && !awaiter->op_.path_.empty()) {
        err = frame_encoder.encode_path(awaiter->op_.path_);
    }
    if (err != common::IoErr::None) {
        frame_encoder.abort();
        request->on_header_send_complete(awaiter, err);
        return err;
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

    request->request_headers_sent_ = true;
    if (awaiter->op_.end_stream_) {
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
    if (!awaiter || !awaiter->owner()) {
        return common::IoErr::Invalid;
    }

    auto *request = awaiter->owner();
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() ||
        request->stream_.remote_rst()) {
        request->on_body_send_complete(awaiter, request->abort_reason_ != common::IoErr::None
                                                        ? request->abort_reason_
                                                        : common::IoErr::Canceled);
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    awaiter->clear_waiting_stream_window();

    const std::size_t remaining = awaiter->op_.chunk_.readable_bytes();
    const std::int32_t stream_window = stream.send_window();
    const std::uint32_t stream_budget = stream_window > 0 ? static_cast<std::uint32_t>(stream_window) : 0U;
    const std::uint32_t conn_budget =
            req.conn_window_budget > 0 ? static_cast<std::uint32_t>(req.conn_window_budget) : 0U;

    if (remaining == 0) {
        if (!awaiter->op_.chunk_.complete()) {
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
        common::IoErr err = frame_encoder.encode(target, awaiter->op_.chunk_, 0);
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
            .end_stream = awaiter->op_.chunk_.complete() && payload_budget == remaining,
    });
    common::IoErr err = frame_encoder.encode(target, awaiter->op_.chunk_, payload_budget);
    if (err != common::IoErr::None) {
        request->on_body_send_complete(awaiter, err);
        return err;
    }

    const std::size_t after_remaining = awaiter->op_.chunk_.readable_bytes();
    result.status = Http2OutboundEncodeResult::Status::Encoded;
    result.consumed_conn_window = static_cast<std::uint32_t>(payload_budget);

    if (after_remaining == 0) {
        if (awaiter->op_.chunk_.complete()) {
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

    awaiter->mark_waiting_stream_window();
    result.next_kind = Http2OutboundNextKind::None;
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::encode_trailer_frames(Http2Stream &stream, void *ctx,
                                                        const Http2OutboundEncodeRequest &req,
                                                        Http2OutboundEncodeTarget &target,
                                                        Http2OutboundEncodeResult &result) noexcept {
    auto *awaiter = static_cast<TrailerSendAwaiter *>(ctx);
    if (!awaiter || !awaiter->owner()) {
        return common::IoErr::Invalid;
    }

    auto *request = awaiter->owner();
    if (request->abort_reason_ != common::IoErr::None || request->stream_.local_rst() ||
        request->stream_.remote_rst()) {
        request->on_trailer_send_complete(awaiter, request->abort_reason_ != common::IoErr::None
                                                           ? request->abort_reason_
                                                           : common::IoErr::Canceled);
        result.status = Http2OutboundEncodeResult::Status::Closed;
        result.next_kind = Http2OutboundNextKind::None;
        return common::IoErr::None;
    }

    Http2HeadersFrameEncoder frame_encoder(
            request->conn_->outbound_hpack_encoder(),
            {
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
    if (abort_reason_ != common::IoErr::None) {
        return false;
    }
    return conn_ != nullptr && conn_->cancel_queued_stream_send(stream_);
}

common::IoErr ClientHttp2Request::handle_status(std::string_view value) noexcept {
    if (reading_trailers_ || current_block_has_status_ || value.size() != 3) {
        return common::IoErr::Invalid;
    }
    int status_code = 0;
    for (char ch: value) {
        if (ch < '0' || ch > '9') {
            return common::IoErr::Invalid;
        }
        status_code = status_code * 10 + (ch - '0');
    }
    current_header_node_->head.status_code = status_code;
    current_block_has_status_ = true;
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                               bool name_stable, bool value_stable) noexcept {
    if (current_header_node_ == nullptr) {
        return common::IoErr::Invalid;
    }
    if (!name.empty() && name.front() == ':') {
        if (name == ":status") {
            return handle_status(value);
        }
        return common::IoErr::Invalid;
    }
    return commit_regular_header(name, name_hash, value, name_stable, value_stable);
}

common::IoErr ClientHttp2Request::commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                        std::string_view value, bool name_stable,
                                                        bool value_stable) noexcept {
    if (!current_header_node_) {
        return common::IoErr::Invalid;
    }
    std::string_view name_copy = name_stable ? name : copy_to_pool(name);
    if (!name_copy.data() && !name.empty()) {
        return common::IoErr::NoMem;
    }
    std::string_view value_copy = value_stable ? value : copy_to_pool(value);
    if (!value_copy.data() && !value.empty()) {
        return common::IoErr::NoMem;
    }
    if (current_header_node_->head.headers.add_view(name_copy, value_copy, const_cast<char *>(name_copy.data()),
                                                    name_hash) == nullptr) {
        return common::IoErr::NoMem;
    }
    saw_regular_header_in_block_ = true;
    return common::IoErr::None;
}

common::IoErr ClientHttp2Request::materialize_name_raw(const std::uint8_t *data, std::size_t len, std::string_view &out,
                                                       std::uint64_t &name_hash) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    return detail::materialize_name_raw(*pool_, data, len, out, name_hash);
}

common::IoErr ClientHttp2Request::materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                           std::string_view &out, std::uint64_t &name_hash) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    return detail::materialize_name_huffman(*pool_, data, len, out, name_hash);
}

common::IoErr ClientHttp2Request::materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                        std::string_view &out) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    return detail::materialize_value_raw(*pool_, data, len, out);
}

common::IoErr ClientHttp2Request::materialize_value_huffman(const std::uint8_t *data, std::size_t len,
                                                            std::string_view &out) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    return detail::materialize_value_huffman(*pool_, data, len, out);
}

std::string_view ClientHttp2Request::copy_to_pool(std::string_view value) noexcept {
    FIBER_ASSERT(pool_ != nullptr);
    return detail::copy_to_pool(*pool_, value);
}

void ClientHttp2Request::on_send_complete(SendAwaiter *awaiter, common::IoErr result) noexcept {
    if (!awaiter || send_awaiter_ != awaiter) {
        return;
    }
    awaiter->complete(result);
}

void ClientHttp2Request::on_header_send_complete(HeaderSendAwaiter *awaiter, common::IoErr result) noexcept {
    on_send_complete(awaiter, result);
}

void ClientHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_abort(abort_reason_);
    }
    response_body_recv_.abort(abort_reason_);
    response_header_recv_.abort(abort_reason_);
}

void ClientHttp2Request::on_body_send_complete(BodySendAwaiter *awaiter, common::IoErr result) noexcept {
    on_send_complete(awaiter, result);
}

void ClientHttp2Request::on_trailer_send_complete(TrailerSendAwaiter *awaiter, common::IoErr result) noexcept {
    on_send_complete(awaiter, result);
}

void ClientHttp2Request::on_stream_send_window_available() noexcept {
    if (send_awaiter_ != nullptr) {
        send_awaiter_->on_stream_send_window_available();
    }
}

void ClientHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Request *>(owner); }

} // namespace fiber::http
