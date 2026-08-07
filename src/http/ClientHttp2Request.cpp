#include <fiber/http/ClientHttp2Request.h>

#include <algorithm>
#include <coroutine>
#include <cstring>
#include <new>

#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/ClientHttp2Push.h>
#include <fiber/http/Http2Connection.h>
#include <fiber/http/Http2DataFrameEncoder.h>
#include <fiber/http/Http2HeadersFrameEncoder.h>
#include <fiber/http/detail/Http2HeaderDecodeUtil.h>

namespace fiber::http {

namespace {

bool is_status_informational(int status_code) noexcept { return status_code >= 100 && status_code < 200; }

bool is_valid_status_code(int status_code) noexcept { return status_code >= 100 && status_code <= 999; }

bool is_terminal_request_write_error(common::IoErr error) noexcept {
    switch (error) {
        case common::IoErr::None:
        case common::IoErr::Invalid:
        case common::IoErr::Busy:
        case common::IoErr::Already:
        case common::IoErr::NoMem:
        case common::IoErr::MessageTooLarge:
        case common::IoErr::NotSupported:
            return false;
        default:
            return true;
    }
}

} // namespace

struct ClientHttp2Request::SendRequestHeaderOp {
    using SuccessType = void;

    SendRequestHeaderOp(const Http2RequestHead &head, bool end_stream) noexcept :
        method_(head.method), scheme_(head.scheme), authority_(head.authority), path_(head.path),
        protocol_(head.protocol), headers_(head.headers), end_stream_(end_stream) {}

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Headers);
    }

    void on_send_done(ClientHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ClientHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

    HttpMethod method_ = HttpMethod::Unknown;
    std::string_view scheme_{};
    std::string_view authority_{};
    std::string_view path_{};
    std::string_view protocol_{};
    const HttpHeaders *headers_ = nullptr;
    bool end_stream_ = false;
};

struct ClientHttp2Request::SendRequestBodyAllOp {
    using SuccessType = std::size_t;

    explicit SendRequestBodyAllOp(mem::IoBufChain &&chunk) noexcept :
        chunk_(std::move(chunk)), total_bytes_(chunk_.readable_bytes()) {}

    [[nodiscard]] bool should_complete_without_submit() const noexcept {
        return chunk_.readable_bytes() == 0 && !chunk_.complete();
    }

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Data);
    }

    [[nodiscard]] std::size_t pending_flow_controlled_bytes() const noexcept { return chunk_.readable_bytes(); }
    void on_send_done(ClientHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ClientHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

    [[nodiscard]] std::size_t success_result() const noexcept { return total_bytes_; }

    mem::IoBufChain chunk_;
    std::size_t total_bytes_ = 0;
};

struct ClientHttp2Request::SendRequestBodySomeOp {
    using SuccessType = std::size_t;
    inline static constexpr bool kAllowsPartialFinalBatch = true;

    explicit SendRequestBodySomeOp(mem::IoBufChain &chunk) noexcept :
        chunk_(&chunk), total_bytes_(chunk.readable_bytes()), end_stream_(chunk.complete()) {}

    SendRequestBodySomeOp(const std::uint8_t *buf, std::size_t len, bool end_stream) noexcept :
        buf_(buf), total_bytes_(len), end_stream_(end_stream) {}

    [[nodiscard]] bool should_complete_without_submit() const noexcept { return total_bytes_ == 0 && !end_stream_; }

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Data);
    }

    [[nodiscard]] std::size_t pending_flow_controlled_bytes() const noexcept { return total_bytes_; }
    void on_send_done(ClientHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ClientHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

    [[nodiscard]] std::size_t success_result() const noexcept { return accepted_bytes_; }

    mem::IoBufChain *chunk_ = nullptr;
    const std::uint8_t *buf_ = nullptr;
    std::size_t total_bytes_ = 0;
    std::size_t accepted_bytes_ = 0;
    bool end_stream_ = false;
};

struct ClientHttp2Request::SendRequestTrailerOp {
    using SuccessType = void;

    explicit SendRequestTrailerOp(const HttpHeaders &headers) noexcept : headers_(&headers) {}

    [[nodiscard]] common::IoErr submit(ClientHttp2Request &request) noexcept {
        return request.conn_->request_stream_send(request.stream_, Http2OutboundKind::Headers);
    }

    void on_send_done(ClientHttp2Request &request, std::uint32_t flow_controlled_bytes,
                      bool operation_final_batch) noexcept;

    common::IoErr on_encode(ClientHttp2Request &request, Http2Stream &stream, const Http2OutboundEncodeRequest &req,
                            Http2OutboundEncodeTarget &target, Http2OutboundEncodeResult &result) noexcept;

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

Http2ExtendedConnectSupport ClientHttp2Request::extended_connect_support() const noexcept {
    if (!conn_ || !conn_->peer_settings_received()) {
        return Http2ExtendedConnectSupport::Unknown;
    }
    return conn_->peer_enable_connect_protocol() ? Http2ExtendedConnectSupport::Enabled
                                                 : Http2ExtendedConnectSupport::Disabled;
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

    co_return co_await HeaderSendAwaiter(*this, timeout, head, end_stream);
}

fiber::async::Task<common::IoResult<std::size_t>>
ClientHttp2Request::write_all(mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept {
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

    auto result = co_await BodyWriteAllAwaiter(*this, timeout, std::move(chunk));
    if (!result) {
        record_request_write_error(result.error());
    }
    co_return result;
}

fiber::async::Task<common::IoResult<std::size_t>>
ClientHttp2Request::write(mem::IoBufChain &chunk, std::chrono::milliseconds timeout) noexcept {
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

    auto result = co_await BodyWriteSomeAwaiter(*this, timeout, chunk);
    if (!result) {
        record_request_write_error(result.error());
    }
    co_return result;
}

fiber::async::Task<common::IoResult<std::size_t>>
ClientHttp2Request::write(const std::uint8_t *buf, std::size_t len, bool end_stream,
                          std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
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

    auto result = co_await BodyWriteSomeAwaiter(*this, timeout, buf, len, end_stream);
    if (!result) {
        record_request_write_error(result.error());
    }
    co_return result;
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

    co_return co_await TrailerSendAwaiter(*this, timeout, headers);
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

common::IoErr ClientHttp2Request::SendRequestHeaderOp::on_encode(ClientHttp2Request &request, Http2Stream &stream,
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

    err = frame_encoder.encode_method(method_);
    if (err == common::IoErr::None && !scheme_.empty()) {
        err = frame_encoder.encode_scheme(scheme_);
    }
    if (err == common::IoErr::None && !authority_.empty()) {
        err = frame_encoder.encode_authority(authority_);
    }
    if (err == common::IoErr::None && !path_.empty()) {
        err = frame_encoder.encode_path(path_);
    }
    if (err == common::IoErr::None && !protocol_.empty()) {
        err = frame_encoder.encode_protocol(protocol_);
    }
    if (err != common::IoErr::None) {
        frame_encoder.abort();
        return err;
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

void ClientHttp2Request::SendRequestHeaderOp::on_send_done(ClientHttp2Request &request,
                                                           std::uint32_t flow_controlled_bytes,
                                                           bool operation_final_batch) noexcept {
    FIBER_ASSERT(flow_controlled_bytes == 0);
    FIBER_ASSERT(operation_final_batch);
    request.request_headers_sent_ = true;
    if (end_stream_) {
        request.stream_.local_end_stream_ = true;
        request.request_finished_ = true;
    }
}

common::IoErr ClientHttp2Request::SendRequestBodyAllOp::on_encode(ClientHttp2Request &request, Http2Stream &stream,
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

void ClientHttp2Request::SendRequestBodyAllOp::on_send_done(ClientHttp2Request &request, std::uint32_t,
                                                            bool operation_final_batch) noexcept {
    if (!operation_final_batch) {
        return;
    }
    if (chunk_.complete()) {
        request.stream_.local_end_stream_ = true;
        request.request_finished_ = true;
    }
}

common::IoErr ClientHttp2Request::SendRequestBodySomeOp::on_encode(ClientHttp2Request &request, Http2Stream &stream,
                                                                   const Http2OutboundEncodeRequest &req,
                                                                   Http2OutboundEncodeTarget &target,
                                                                   Http2OutboundEncodeResult &result) noexcept {
    if (request.abort_reason_ != common::IoErr::None || request.stream_.local_rst() || request.stream_.remote_rst()) {
        return request.abort_reason_ != common::IoErr::None ? request.abort_reason_ : common::IoErr::Canceled;
    }

    if (total_bytes_ == 0) {
        FIBER_ASSERT(end_stream_);
        mem::IoBufChain empty(request.node_pool());
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
        staged.bind_node_pool(request.node_pool());
        mem::IoBuf owned = mem::IoBuf::allocate(payload_bytes);
        if (!owned) {
            return common::IoErr::NoMem;
        }
        std::memcpy(owned.writable_data(), buf_, payload_bytes);
        owned.commit(payload_bytes);
        if (!staged.append(std::move(owned))) {
            return common::IoErr::NoMem;
        }
        if (end_stream_ && payload_bytes == total_bytes_) {
            staged.mark_complete();
        }
        payload = &staged;
    } else if (!chunk_->bound() || &chunk_->node_pool() != &request.node_pool()) {
        staged.bind_node_pool(request.node_pool());
        if (!chunk_->retain_prefix(payload_bytes, staged)) {
            return common::IoErr::NoMem;
        }
        payload = &staged;
        consume_borrowed_chain = true;
    }

    Http2DataFrameEncoder frame_encoder({
            .stream_id = stream.stream_id(),
            .max_frame_size = req.max_frame_size,
            .end_stream = end_stream_ && payload_bytes == total_bytes_,
    });
    common::IoErr err = frame_encoder.encode(target, *payload, payload_bytes);
    if (err != common::IoErr::None) {
        return err;
    }
    if (consume_borrowed_chain) {
        chunk_->consume_and_compact(payload_bytes);
        if (payload_bytes == total_bytes_ && end_stream_) {
            chunk_->clear_complete();
        }
    } else if (chunk_ != nullptr && payload_bytes == total_bytes_ && end_stream_) {
        chunk_->clear_complete();
    }

    accepted_bytes_ = payload_bytes;
    result.flow_controlled_bytes = static_cast<std::uint32_t>(payload_bytes);
    result.operation_final_batch = true;
    return common::IoErr::None;
}

void ClientHttp2Request::SendRequestBodySomeOp::on_send_done(ClientHttp2Request &request,
                                                             std::uint32_t flow_controlled_bytes,
                                                             bool operation_final_batch) noexcept {
    FIBER_ASSERT(operation_final_batch);
    FIBER_ASSERT(flow_controlled_bytes == accepted_bytes_);
    if (end_stream_ && accepted_bytes_ == total_bytes_) {
        request.stream_.local_end_stream_ = true;
        request.request_finished_ = true;
    }
}

common::IoErr ClientHttp2Request::SendRequestTrailerOp::on_encode(ClientHttp2Request &request, Http2Stream &stream,
                                                                  const Http2OutboundEncodeRequest &req,
                                                                  Http2OutboundEncodeTarget &target,
                                                                  Http2OutboundEncodeResult &result) noexcept {
    if (request.abort_reason_ != common::IoErr::None || request.stream_.local_rst() || request.stream_.remote_rst()) {
        return request.abort_reason_ != common::IoErr::None ? request.abort_reason_ : common::IoErr::Canceled;
    }

    Http2HeadersFrameEncoder frame_encoder({
            .stream_id = stream.stream_id(),
            .max_frame_size = req.max_frame_size,
            .end_stream = true,
            .hpack = {.max_string_size = request.conn_->options_.max_hpack_string_size},
    });
    common::IoErr err = frame_encoder.begin(target);
    if (err != common::IoErr::None) {
        return err;
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

void ClientHttp2Request::SendRequestTrailerOp::on_send_done(ClientHttp2Request &request,
                                                            std::uint32_t flow_controlled_bytes,
                                                            bool operation_final_batch) noexcept {
    FIBER_ASSERT(flow_controlled_bytes == 0);
    FIBER_ASSERT(operation_final_batch);
    request.stream_.local_end_stream_ = true;
    request.request_finished_ = true;
}

void ClientHttp2Request::on_stream_abort(void *owner, common::IoErr reason) noexcept {
    if (!owner) {
        return;
    }
    static_cast<ClientHttp2Request *>(owner)->on_stream_aborted(reason);
}

bool ClientHttp2Request::cancel_queued_send() noexcept {
    if (abort_reason_ != common::IoErr::None) {
        return false;
    }
    return conn_ != nullptr && conn_->cancel_queued_stream_send(stream_);
}

void ClientHttp2Request::record_request_write_error(common::IoErr error) noexcept {
    if (!is_terminal_request_write_error(error) || abort_reason_ != common::IoErr::None) {
        return;
    }
    (void) stream_.close_rst(Http2ErrorCode::Cancel, error);
    if (abort_reason_ == common::IoErr::None) {
        on_stream_aborted(error);
    }
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
    if (current_header_node_->head.headers.add_view(name_copy, value_copy, name_copy.data(), name_hash) == nullptr) {
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

void ClientHttp2Request::on_stream_aborted(common::IoErr reason) noexcept {
    if (abort_reason_ == common::IoErr::None) {
        abort_reason_ = reason;
    }
    response_body_recv_.abort(abort_reason_);
    response_header_recv_.abort(abort_reason_);
}

void ClientHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ClientHttp2Request *>(owner); }

} // namespace fiber::http
