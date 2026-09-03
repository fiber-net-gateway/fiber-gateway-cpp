#include "http/ServerHttp3Request.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
#include <utility>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/HeaderMap.h>
#include <fiber/http/Http3Codec.h>
#include <fiber/http/Http3Connection.h>
#include <fiber/http/Http3QpackDecoder.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/HttpUriParse.h>
#include "http/Http3FrameWriter.h"
#include "http/Http3QpackEncoderIoBufWriter.h"
#include "http/Huffman.h"

namespace fiber::http {

namespace {

constexpr std::size_t kHttp3RequestReadChunkSize = 128 << 10;

[[nodiscard]] std::uint64_t error_value(Http3ErrorCode error) noexcept { return static_cast<std::uint64_t>(error); }

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
    if (method == "MKCOL") {
        return HttpMethod::MKCOL;
    }
    if (method == "COPY") {
        return HttpMethod::Copy;
    }
    if (method == "MOVE") {
        return HttpMethod::Move;
    }
    if (method == "PROPFIND") {
        return HttpMethod::PropFind;
    }
    if (method == "PROPPATCH") {
        return HttpMethod::PropPatch;
    }
    if (method == "LOCK") {
        return HttpMethod::Lock;
    }
    if (method == "UNLOCK") {
        return HttpMethod::Unlock;
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

bool is_valid_header_name_char(unsigned char ch) noexcept {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
}

bool is_valid_method_char(unsigned char ch) noexcept { return (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '-'; }

bool is_valid_scheme(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto ch = static_cast<unsigned char>(value[i]);
        const auto lower = static_cast<unsigned char>(ch | 0x20U);
        if (lower >= 'a' && lower <= 'z') {
            continue;
        }
        if (i != 0 && ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.')) {
            continue;
        }
        return false;
    }
    return true;
}

bool equals_ascii_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        auto a = static_cast<unsigned char>(left[i]);
        auto b = static_cast<unsigned char>(right[i]);
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<unsigned char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<unsigned char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool is_forbidden_request_stream_frame(std::uint64_t type) noexcept {
    switch (static_cast<Http3FrameType>(type)) {
        case Http3FrameType::CancelPush:
        case Http3FrameType::Settings:
        case Http3FrameType::PushPromise:
        case Http3FrameType::Goaway:
        case Http3FrameType::MaxPushId:
            return true;
        case Http3FrameType::Data:
        case Http3FrameType::Headers:
            return false;
    }
    return false;
}

bool response_must_not_have_body(const HttpExchange &exchange, int status_code) noexcept {
    return exchange.method() == HttpMethod::Head || (status_code >= 100 && status_code < 200) || status_code == 204 ||
           status_code == 304;
}

} // namespace

enum class ServerHttp3Request::HeaderBlockTarget : std::uint8_t {
    Request,
    Trailer,
};

enum class ServerHttp3Request::BodyRecvState : std::uint8_t {
    FrameHeader,
    DataPayload,
    TrailerPayload,
    WaitFin,
    Complete,
    Error,
};

ServerHttp3Request::ServerHttp3Request(Http3Connection &conn, const HttpServerOptions &http_options,
                                       const HttpHandler &handler,
                                       std::shared_ptr<const HttpHandler> handler_owner) noexcept :
    quic_lease_(conn.quic().lease()), stream_(this, &ServerHttp3Request::destroy_owner),
    inbound_buf_(conn.quic().recv_extent_pool()), exchange_(conn.quic().recv_extent_pool(), conn.quic().remote_addr()),
    handler_(&handler), handler_owner_(std::move(handler_owner)),
    max_qpack_string_size_(static_cast<std::uint32_t>(
            std::min<std::size_t>(http_options.header_large_size, std::numeric_limits<std::uint32_t>::max()))),
    body_timeout_(http_options.body_timeout), body_recv_state_(BodyRecvState::FrameHeader),
    extended_connect_enabled_(conn.local_settings().enable_connect_protocol) {
    // HTTP/3 request bodies are stream-delimited when Content-Length is absent.
    exchange_.request_body_spec_ = HttpBodySpec::Stream();
}

quic::QuicStream::Lease ServerHttp3Request::create(std::uint64_t stream_id, Http3Connection &conn,
                                                   const HttpServerOptions &http_options,
                                                   const HttpHandler &handler) noexcept {
    (void) stream_id;
    auto *request = new (std::nothrow) ServerHttp3Request(conn, http_options, handler);
    if (request == nullptr) {
        return {};
    }
    return quic::QuicStream::Lease::adopt(&request->stream_);
}

quic::QuicStream::Lease ServerHttp3Request::create(std::uint64_t stream_id, Http3Connection &conn,
                                                   const HttpServerOptions &http_options,
                                                   std::shared_ptr<const HttpHandler> handler) noexcept {
    (void) stream_id;
    if (!handler) {
        return {};
    }
    const HttpHandler *handler_ptr = handler.get();
    auto *request = new (std::nothrow) ServerHttp3Request(conn, http_options, *handler_ptr, std::move(handler));
    if (request == nullptr) {
        return {};
    }
    return quic::QuicStream::Lease::adopt(&request->stream_);
}

ServerHttp3Request *ServerHttp3Request::from_stream(quic::QuicStream &stream) noexcept {
    if (stream.destroy_callback() != &ServerHttp3Request::destroy_owner) {
        return nullptr;
    }
    auto *request = static_cast<ServerHttp3Request *>(stream.owner());
    if (request == nullptr || &request->stream_ != &stream) {
        return nullptr;
    }
    return request;
}

const ServerHttp3Request *ServerHttp3Request::from_stream(const quic::QuicStream &stream) noexcept {
    if (stream.destroy_callback() != &ServerHttp3Request::destroy_owner) {
        return nullptr;
    }
    auto *request = static_cast<const ServerHttp3Request *>(stream.owner());
    if (request == nullptr || &request->stream_ != &stream) {
        return nullptr;
    }
    return request;
}

void ServerHttp3Request::start_read_loop(event::EventLoop &loop) noexcept {
    if (read_loop_started_) {
        return;
    }
    read_loop_started_ = true;
    async::spawn(loop, [this, lease = stream_.lease()]() mutable { return run_read_loop(std::move(lease)); });
}

void ServerHttp3Request::destroy_owner(void *owner, quic::QuicStream &) noexcept {
    delete static_cast<ServerHttp3Request *>(owner);
}

class ServerHttp3Request::HeaderBlockParser final : public common::NonCopyable, public common::NonMovable {
public:
    HeaderBlockParser(ServerHttp3Request &request, HeaderBlockTarget target) noexcept :
        request_(request), target_(target) {}

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] common::IoErr decode(const std::uint8_t *data, std::size_t len, bool end_block) noexcept;

private:
    enum PseudoHeaderSeen : std::uint8_t {
        MethodSeen = 1U << 0U,
        PathSeen = 1U << 1U,
        SchemeSeen = 1U << 2U,
        AuthoritySeen = 1U << 3U,
        ProtocolSeen = 1U << 4U,
    };

    using PseudoHeaderHandler = common::IoErr (HeaderBlockParser::*)(std::string_view value) noexcept;
    using RegularHeaderHandler = common::IoErr (HeaderBlockParser::*)(std::string_view value) noexcept;

    struct PseudoHeaderRule {
        std::uint8_t seen_bit = 0;
        PseudoHeaderHandler handler = nullptr;
    };

    [[nodiscard]] static const Http3QpackDecoder::Ops &decoder_ops() noexcept;
    [[nodiscard]] static const HeaderMap<PseudoHeaderRule> &pseudo_header_map() noexcept;
    [[nodiscard]] static const HeaderMap<RegularHeaderHandler> &regular_header_handler_map() noexcept;
    static common::IoErr on_indexed_field(void *owner, Http3QpackDecoder::TableEntryView entry) noexcept;
    static common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept;
    static common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;

    [[nodiscard]] common::IoErr complete_header_block() noexcept;
    [[nodiscard]] common::IoErr fail(Http3ErrorCode error, common::IoErr reason = common::IoErr::Invalid) noexcept;
    [[nodiscard]] common::IoErr qpack_decode_failed(common::IoErr reason) noexcept;
    [[nodiscard]] common::IoErr validate_field(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr finalize_pseudo_headers() noexcept;
    [[nodiscard]] common::IoErr materialize_name_raw(const std::uint8_t *data, std::size_t len, std::string_view &out,
                                                     std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                         std::string_view &out, std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                      std::string_view &out) noexcept;
    [[nodiscard]] common::IoErr materialize_value_huffman(const std::uint8_t *data, std::size_t len,
                                                          std::string_view &out) noexcept;
    // name/value must remain valid for the HttpExchange lifetime. They either
    // reference static storage or memory owned by exchange_.pool_.
    [[nodiscard]] common::IoErr commit_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value) noexcept;
    [[nodiscard]] common::IoErr commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                      std::string_view value) noexcept;
    [[nodiscard]] common::IoErr apply_regular_header_policy(std::string_view name, std::uint64_t name_hash,
                                                            std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_pseudo_header(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_method(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_path(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_scheme(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_authority(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_protocol(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_content_length(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_forbidden_regular_header(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_te(std::string_view value) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(const std::uint8_t *data, std::size_t len) noexcept;

    ServerHttp3Request &request_;
    HeaderBlockTarget target_ = HeaderBlockTarget::Request;
    Http3QpackDecoder qpack_decoder_;
    std::string_view pending_name_;
    std::uint64_t pending_name_hash_ = 0;
    bool saw_regular_header_in_block_ = false;
    bool te_seen_ = false;
    std::uint8_t pseudo_seen_ = 0;
    std::string_view path_;
    std::string_view authority_;
    HttpUriParseState uri_state_{};
};

const Http3QpackDecoder::Ops &ServerHttp3Request::HeaderBlockParser::decoder_ops() noexcept {
    static const Http3QpackDecoder::Ops kOps{
            &HeaderBlockParser::on_indexed_field, &HeaderBlockParser::on_indexed_name,
            &HeaderBlockParser::on_name_raw,      &HeaderBlockParser::on_name_huffman,
            &HeaderBlockParser::on_value_raw,     &HeaderBlockParser::on_value_huffman,
    };
    return kOps;
}

const HeaderMap<ServerHttp3Request::HeaderBlockParser::PseudoHeaderRule> &
ServerHttp3Request::HeaderBlockParser::pseudo_header_map() noexcept {
    static HeaderMap<PseudoHeaderRule> handlers = []() {
        HeaderMap<PseudoHeaderRule>::Builder builder(5);
        builder.insert(":method", PseudoHeaderRule{MethodSeen, &HeaderBlockParser::handle_method});
        builder.insert(":path", PseudoHeaderRule{PathSeen, &HeaderBlockParser::handle_path});
        builder.insert(":scheme", PseudoHeaderRule{SchemeSeen, &HeaderBlockParser::handle_scheme});
        builder.insert(":authority", PseudoHeaderRule{AuthoritySeen, &HeaderBlockParser::handle_authority});
        builder.insert(":protocol", PseudoHeaderRule{ProtocolSeen, &HeaderBlockParser::handle_protocol});
        return std::move(builder).build();
    }();
    return handlers;
}

const HeaderMap<ServerHttp3Request::HeaderBlockParser::RegularHeaderHandler> &
ServerHttp3Request::HeaderBlockParser::regular_header_handler_map() noexcept {
    static HeaderMap<RegularHeaderHandler> handlers = []() {
        HeaderMap<RegularHeaderHandler>::Builder builder(6);
        builder.insert("content-length", &HeaderBlockParser::handle_content_length);
        builder.insert("connection", &HeaderBlockParser::handle_forbidden_regular_header);
        builder.insert("keep-alive", &HeaderBlockParser::handle_forbidden_regular_header);
        builder.insert("transfer-encoding", &HeaderBlockParser::handle_forbidden_regular_header);
        builder.insert("upgrade", &HeaderBlockParser::handle_forbidden_regular_header);
        builder.insert("te", &HeaderBlockParser::handle_te);
        return std::move(builder).build();
    }();
    return handlers;
}

bool ServerHttp3Request::HeaderBlockParser::init() noexcept {
    if (!qpack_decoder_.init(request_.max_qpack_string_size_)) {
        return false;
    }
    pending_name_ = {};
    pending_name_hash_ = 0;
    saw_regular_header_in_block_ = false;
    qpack_decoder_.begin_block(this, &decoder_ops());
    return true;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::decode(const std::uint8_t *data, std::size_t len,
                                                            bool end_block) noexcept {
    common::IoErr err = qpack_decoder_.decode(data, len, end_block);
    if (err != common::IoErr::None) {
        return qpack_decode_failed(err);
    }
    if (!end_block) {
        return common::IoErr::None;
    }
    return complete_header_block();
}

common::IoErr ServerHttp3Request::HeaderBlockParser::complete_header_block() noexcept {
    if (pending_name_.data() != nullptr) {
        return fail(Http3ErrorCode::MessageError);
    }
    if (target_ == HeaderBlockTarget::Trailer) {
        request_.exchange_.request_trailers_complete_ = true;
        return common::IoErr::None;
    }

    if (request_.request_head_received_) {
        return fail(Http3ErrorCode::MessageError);
    }
    common::IoErr err = finalize_pseudo_headers();
    if (err != common::IoErr::None) {
        return err;
    }

    request_.request_head_received_ = true;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::fail(Http3ErrorCode error, common::IoErr reason) noexcept {
    request_.request_parse_error_ = error;
    return reason == common::IoErr::None ? common::IoErr::Invalid : reason;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::qpack_decode_failed(common::IoErr reason) noexcept {
    if (request_.request_parse_error_ == Http3ErrorCode::GeneralProtocolError) {
        return fail(Http3ErrorCode::QpackDecompressionFailed, reason);
    }
    return reason == common::IoErr::None ? common::IoErr::Invalid : reason;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::validate_field(std::string_view name,
                                                                    std::string_view value) noexcept {
    if (name.empty()) {
        return fail(Http3ErrorCode::MessageError);
    }

    const std::size_t start = is_pseudo_header(name) ? 1U : 0U;
    for (std::size_t i = start; i < name.size(); ++i) {
        if (!is_valid_header_name_char(static_cast<unsigned char>(name[i]))) {
            return fail(Http3ErrorCode::MessageError);
        }
    }

    for (char ch: value) {
        if (ch == '\0' || ch == '\r' || ch == '\n') {
            return fail(Http3ErrorCode::MessageError);
        }
    }

    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::finalize_pseudo_headers() noexcept {
    constexpr std::uint8_t kRequired = MethodSeen | PathSeen | SchemeSeen;
    if ((pseudo_seen_ & kRequired) != kRequired) {
        return fail(Http3ErrorCode::MessageError);
    }

    common::IoErr err = http_process_uri(path_, uri_state_, request_.exchange_.uri_, &request_.exchange_.pool());
    if (err != common::IoErr::None) {
        return fail(Http3ErrorCode::MessageError, err);
    }

    const HttpHeaders::HeaderField *host = request_.exchange_.host_header();
    if ((pseudo_seen_ & AuthoritySeen) != 0) {
        if (host != nullptr) {
            if (host->value_view() != authority_) {
                return fail(Http3ErrorCode::MessageError);
            }
        } else {
            err = commit_regular_header("host", http_header_name_hash("host"), authority_);
            if (err != common::IoErr::None) {
                return err;
            }
            host = request_.exchange_.host_header();
        }
    }

    if (host == nullptr || host->value_len == 0) {
        return fail(Http3ErrorCode::MessageError);
    }

    request_.exchange_.version_ = HttpVersion::HTTP_3_0;
    return common::IoErr::None;
}

common::IoErr
ServerHttp3Request::HeaderBlockParser::on_indexed_field(void *owner, Http3QpackDecoder::TableEntryView entry) noexcept {
    auto *parser = static_cast<HeaderBlockParser *>(owner);
    return parser->commit_field(entry.name, entry.name_hash, entry.value);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::on_indexed_name(void *owner, std::string_view name,
                                                                     std::uint64_t name_hash) noexcept {
    auto *parser = static_cast<HeaderBlockParser *>(owner);
    parser->pending_name_ = name;
    parser->pending_name_hash_ = name_hash;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::on_name_raw(void *owner, const std::uint8_t *data,
                                                                 std::size_t len) noexcept {
    auto *parser = static_cast<HeaderBlockParser *>(owner);
    std::string_view name;
    std::uint64_t name_hash = 0;
    common::IoErr err = parser->materialize_name_raw(data, len, name, name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    parser->pending_name_ = name;
    parser->pending_name_hash_ = name_hash;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::on_name_huffman(void *owner, const std::uint8_t *data,
                                                                     std::size_t len) noexcept {
    auto *parser = static_cast<HeaderBlockParser *>(owner);
    std::string_view name;
    std::uint64_t name_hash = 0;
    common::IoErr err = parser->materialize_name_huffman(data, len, name, name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    parser->pending_name_ = name;
    parser->pending_name_hash_ = name_hash;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::on_value_raw(void *owner, const std::uint8_t *data,
                                                                  std::size_t len) noexcept {
    auto *parser = static_cast<HeaderBlockParser *>(owner);
    std::string_view value;
    common::IoErr err = parser->materialize_value_raw(data, len, value);
    if (err != common::IoErr::None) {
        return err;
    }
    if (parser->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    std::string_view pending_name = parser->pending_name_;
    std::uint64_t pending_name_hash = parser->pending_name_hash_;
    parser->pending_name_ = {};
    parser->pending_name_hash_ = 0;
    return parser->commit_field(pending_name, pending_name_hash, value);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::on_value_huffman(void *owner, const std::uint8_t *data,
                                                                      std::size_t len) noexcept {
    auto *parser = static_cast<HeaderBlockParser *>(owner);
    std::string_view value;
    common::IoErr err = parser->materialize_value_huffman(data, len, value);
    if (err != common::IoErr::None) {
        return err;
    }
    if (parser->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    std::string_view pending_name = parser->pending_name_;
    std::uint64_t pending_name_hash = parser->pending_name_hash_;
    parser->pending_name_ = {};
    parser->pending_name_hash_ = 0;
    return parser->commit_field(pending_name, pending_name_hash, value);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::materialize_name_raw(const std::uint8_t *data, std::size_t len,
                                                                          std::string_view &out,
                                                                          std::uint64_t &name_hash) noexcept {
    out = copy_to_pool(data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    name_hash = http_header_name_hash(out);
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                                              std::string_view &out,
                                                                              std::uint64_t &name_hash) noexcept {
    out = {};
    bool ok = false;
    std::size_t decoded_len = hpack_huffman_decoded_length(data, len, &ok);
    if (!ok) {
        return common::IoErr::Invalid;
    }
    if (decoded_len != 0) {
        auto *mem = static_cast<char *>(request_.exchange_.pool_.alloc(decoded_len));
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

common::IoErr ServerHttp3Request::HeaderBlockParser::materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                                           std::string_view &out) noexcept {
    out = copy_to_pool(data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::materialize_value_huffman(const std::uint8_t *data,
                                                                               std::size_t len,
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

    auto *mem = static_cast<char *>(request_.exchange_.pool_.alloc(decoded_len));
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

common::IoErr ServerHttp3Request::HeaderBlockParser::commit_field(std::string_view name, std::uint64_t name_hash,
                                                                  std::string_view value) noexcept {
    common::IoErr err = validate_field(name, value);
    if (err != common::IoErr::None) {
        return err;
    }

    if (is_pseudo_header(name)) {
        if (target_ == HeaderBlockTarget::Trailer || saw_regular_header_in_block_) {
            return fail(Http3ErrorCode::MessageError);
        }
        return handle_pseudo_header(name, value);
    }

    saw_regular_header_in_block_ = true;
    return commit_regular_header(name, name_hash, value);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::commit_regular_header(std::string_view name,
                                                                           std::uint64_t name_hash,
                                                                           std::string_view value) noexcept {
    if (target_ == HeaderBlockTarget::Request) {
        common::IoErr err = apply_regular_header_policy(name, name_hash, value);
        if (err != common::IoErr::None) {
            return err;
        }
    }

    HttpHeaders &target = target_ == HeaderBlockTarget::Trailer ? request_.exchange_.request_trailers_
                                                                : request_.exchange_.request_headers_;
    char *lowcase_name = name.empty() ? nullptr : const_cast<char *>(name.data());
    auto *field = target.add_view(name, value, lowcase_name, name_hash);
    if (!field) {
        return common::IoErr::NoMem;
    }
    if (target_ == HeaderBlockTarget::Request) {
        request_.exchange_.cache_request_header_field(*field);
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::apply_regular_header_policy(std::string_view name,
                                                                                 std::uint64_t name_hash,
                                                                                 std::string_view value) noexcept {
    auto *handler = regular_header_handler_map().get(name, name_hash);
    if (handler == nullptr) {
        return common::IoErr::None;
    }
    return (this->*(*handler))(value);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_pseudo_header(std::string_view name,
                                                                          std::string_view value) noexcept {
    auto *rule = pseudo_header_map().get(name);
    if (rule == nullptr || rule->handler == nullptr) {
        return fail(Http3ErrorCode::MessageError);
    }
    if ((pseudo_seen_ & rule->seen_bit) != 0) {
        return fail(Http3ErrorCode::MessageError);
    }
    pseudo_seen_ |= rule->seen_bit;
    return (this->*rule->handler)(value);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_method(std::string_view value) noexcept {
    if (value.empty()) {
        return fail(Http3ErrorCode::MessageError);
    }
    for (char ch: value) {
        if (!is_valid_method_char(static_cast<unsigned char>(ch))) {
            return fail(Http3ErrorCode::MessageError);
        }
    }
    request_.exchange_.method_view_ = value;
    request_.exchange_.method_ = parse_method(value);
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_path(std::string_view value) noexcept {
    if (value.empty()) {
        return fail(Http3ErrorCode::MessageError);
    }
    HttpUriParseState uri_state{};
    common::IoErr err = http_parse_uri(value, uri_state);
    if (err != common::IoErr::None) {
        return fail(Http3ErrorCode::MessageError, err);
    }
    path_ = value;
    uri_state_ = uri_state;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_scheme(std::string_view value) noexcept {
    if (!is_valid_scheme(value)) {
        return fail(Http3ErrorCode::MessageError);
    }
    request_.exchange_.scheme_view_ = value;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_authority(std::string_view value) noexcept {
    authority_ = value;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_protocol(std::string_view value) noexcept {
    if (!request_.extended_connect_enabled_) {
        return fail(Http3ErrorCode::MessageError);
    }
    request_.exchange_.protocol_view_ = value;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_content_length(std::string_view value) noexcept {
    unsigned long long parsed = 0;
    const auto *first = value.data();
    const auto *last = value.data() + value.size();
    auto result = std::from_chars(first, last, parsed, 10);
    if (result.ec != std::errc() || result.ptr != last ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
        return fail(Http3ErrorCode::MessageError);
    }

    const std::size_t length = static_cast<std::size_t>(parsed);
    HttpExchange &exchange = request_.exchange_;
    if (exchange.request_body_spec_.is_content_length() && exchange.request_body_spec_.content_length() != length) {
        return fail(Http3ErrorCode::MessageError);
    }
    exchange.request_body_spec_ = HttpBodySpec::ContentLength(length);
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_forbidden_regular_header(std::string_view) noexcept {
    return fail(Http3ErrorCode::MessageError);
}

common::IoErr ServerHttp3Request::HeaderBlockParser::handle_te(std::string_view value) noexcept {
    if (te_seen_ || !equals_ascii_ci(value, "trailers")) {
        return fail(Http3ErrorCode::MessageError);
    }
    te_seen_ = true;
    return common::IoErr::None;
}

std::string_view ServerHttp3Request::HeaderBlockParser::copy_to_pool(const std::uint8_t *data,
                                                                     std::size_t len) noexcept {
    if (len == 0) {
        return {};
    }
    auto *mem = static_cast<char *>(request_.exchange_.pool_.alloc(len));
    if (!mem) {
        return {};
    }
    std::memcpy(mem, data, len);
    return std::string_view(mem, len);
}

async::DetachedTask ServerHttp3Request::run_read_loop(quic::QuicStream::Lease lease) noexcept {
    if (!lease) {
        co_return;
    }

    auto parsed = co_await parse_request_header();
    if (!parsed) {
        stream_.close(error_value(request_parse_error_));
        co_return;
    }
    if (exchange_.request_body_spec_.is_stream() && inbound_buf_.complete() && inbound_buf_.readable_bytes() == 0) {
        exchange_.request_body_spec_ = HttpBodySpec::None();
    }

    if (handler_ == nullptr) {
        stream_.close(error_value(Http3ErrorCode::InternalError));
        co_return;
    }

    exchange_.set_io(this);
    handler_started_ = true;
    co_await (*handler_)(exchange_);
    handler_done_ = true;
    if (response_finished_ && !stream_.recv_closed() && !stream_.stop_sending()) {
        (void) stream_.stop_read(error_value(Http3ErrorCode::RequestCancelled));
    }
    exchange_.set_io(nullptr);

    if (!response_finished_) {
        stream_.close(error_value(Http3ErrorCode::RequestCancelled));
    }
    (void) lease;
    co_return;
}

common::IoErr ServerHttp3Request::set_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                       void *ctx) noexcept {
    return stream_.set_send_aborted_callback(callback, ctx);
}

common::IoErr ServerHttp3Request::clear_response_channel_closed_callback(ResponseChannelClosedCallback callback,
                                                                         void *ctx) noexcept {
    return stream_.clear_send_aborted_callback(callback, ctx);
}

Http3ParseStatus ServerHttp3Request::parse_frame_header_once() noexcept {
    const std::size_t before = inbound_buf_.readable_bytes();
    Http3ParseStatus status = frame_parser_.parse(inbound_buf_);
    if (before != inbound_buf_.readable_bytes()) {
        frame_header_in_progress_ = true;
    }
    if (status == Http3ParseStatus::Done) {
        current_frame_ = frame_parser_.header();
        frame_parser_.reset();
        frame_header_in_progress_ = false;
    }
    return status;
}

common::IoErr ServerHttp3Request::fail_request(Http3ErrorCode error, common::IoErr reason) noexcept {
    request_parse_error_ = error;
    body_recv_state_ = BodyRecvState::Error;
    return reason == common::IoErr::None ? common::IoErr::Invalid : reason;
}

common::IoResult<mem::IoBufChain> ServerHttp3Request::fail_read_body(Http3ErrorCode error,
                                                                     common::IoErr reason) noexcept {
    const common::IoErr err = fail_request(error, reason);
    stream_.close(error_value(error));
    return std::unexpected(err);
}

async::Task<common::IoResult<void>> ServerHttp3Request::read_more_input(std::chrono::milliseconds timeout) noexcept {
    if (inbound_buf_.complete()) {
        co_return common::IoResult<void>{};
    }
    auto read = co_await stream_.read(kHttp3RequestReadChunkSize, inbound_buf_, timeout);
    if (!read) {
        co_return std::unexpected(read.error());
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> ServerHttp3Request::skip_frame_payload(std::uint64_t payload_length,
                                                                           std::chrono::milliseconds timeout) noexcept {
    std::uint64_t remaining = payload_length;
    while (remaining != 0) {
        const std::uint64_t available = static_cast<std::uint64_t>(inbound_buf_.readable_bytes());
        if (available == 0) {
            if (inbound_buf_.complete()) {
                const common::IoErr err = fail_request(Http3ErrorCode::RequestIncomplete);
                co_return std::unexpected(err);
            }
            auto read = co_await read_more_input(timeout);
            if (!read) {
                (void) fail_request(Http3ErrorCode::RequestIncomplete, read.error());
                co_return std::unexpected(read.error());
            }
            continue;
        }

        const std::uint64_t take = std::min(available, remaining);
        inbound_buf_.consume_and_compact(static_cast<std::size_t>(take));
        remaining -= take;
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> ServerHttp3Request::parse_header_block(HeaderBlockTarget target,
                                                                           std::uint64_t payload_length) noexcept {
    HeaderBlockParser parser(*this, target);
    if (!parser.init()) {
        const common::IoErr err = fail_request(Http3ErrorCode::InternalError, common::IoErr::NoMem);
        co_return std::unexpected(err);
    }

    std::uint64_t remaining = payload_length;
    if (remaining == 0) {
        common::IoErr err = parser.decode(nullptr, 0, true);
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        co_return common::IoResult<void>{};
    }

    while (remaining != 0) {
        mem::IoBuf *buf = inbound_buf_.first_readable();
        if (buf == nullptr || buf->readable() == 0) {
            if (inbound_buf_.complete()) {
                const common::IoErr err = fail_request(Http3ErrorCode::RequestIncomplete);
                co_return std::unexpected(err);
            }
            auto read = co_await read_more_input(target == HeaderBlockTarget::Request ? std::chrono::milliseconds::max()
                                                                                      : body_timeout_);
            if (!read) {
                (void) fail_request(Http3ErrorCode::RequestIncomplete, read.error());
                co_return std::unexpected(read.error());
            }
            continue;
        }

        const std::size_t take = std::min<std::uint64_t>(buf->readable(), remaining);
        const bool end_block = static_cast<std::uint64_t>(take) == remaining;
        common::IoErr err = parser.decode(buf->readable_data(), take, end_block);
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        inbound_buf_.consume_and_compact(take);
        remaining -= take;
    }

    co_return common::IoResult<void>{};
}

common::IoResult<void> ServerHttp3Request::take_body_payload(mem::IoBufChain &out, std::size_t bytes) noexcept {
    if (bytes == 0) {
        return common::IoResult<void>{};
    }

    if (!inbound_buf_.take_prefix(bytes, out)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return common::IoResult<void>{};
}

async::Task<common::IoResult<void>> ServerHttp3Request::parse_request_header() noexcept {
    while (!request_head_received_) {
        Http3ParseStatus status = parse_frame_header_once();
        if (status == Http3ParseStatus::NeedMore) {
            if (inbound_buf_.complete()) {
                const common::IoErr err = fail_request(Http3ErrorCode::RequestIncomplete);
                co_return std::unexpected(err);
            }
            auto read = co_await read_more_input(std::chrono::milliseconds::max());
            if (!read) {
                const common::IoErr err = fail_request(Http3ErrorCode::RequestIncomplete, read.error());
                co_return std::unexpected(err);
            }
            continue;
        }
        if (status == Http3ParseStatus::Error) {
            common::IoErr err = fail_request(frame_parser_.error().h3_error, frame_parser_.error().io_error);
            co_return std::unexpected(err);
        }

        const Http3FrameHeader header = current_frame_;
        if (header.type == static_cast<std::uint64_t>(Http3FrameType::Headers)) {
            auto parsed = co_await parse_header_block(HeaderBlockTarget::Request, header.length);
            if (!parsed) {
                co_return std::unexpected(parsed.error());
            }
            body_recv_state_ = BodyRecvState::FrameHeader;
            co_return common::IoResult<void>{};
        }

        if (header.type == static_cast<std::uint64_t>(Http3FrameType::Data) ||
            is_forbidden_request_stream_frame(header.type)) {
            const common::IoErr err = fail_request(Http3ErrorCode::FrameUnexpected);
            co_return std::unexpected(err);
        }

        auto skipped = co_await skip_frame_payload(header.length, std::chrono::milliseconds::max());
        if (!skipped) {
            if (request_parse_error_ == Http3ErrorCode::GeneralProtocolError) {
                (void) fail_request(Http3ErrorCode::RequestIncomplete, skipped.error());
            }
            co_return std::unexpected(skipped.error());
        }
    }

    co_return common::IoResult<void>{};
}

common::IoErr ServerHttp3Request::begin_body_frame(const Http3FrameHeader &header) noexcept {
    if (header.type == static_cast<std::uint64_t>(Http3FrameType::Data)) {
        if (exchange_.request_trailers_complete_) {
            return fail_request(Http3ErrorCode::MessageError);
        }
        frame_payload_remaining_ = header.length;
        body_recv_state_ = BodyRecvState::DataPayload;
        return common::IoErr::None;
    }

    if (header.type == static_cast<std::uint64_t>(Http3FrameType::Headers)) {
        if (exchange_.request_trailers_complete_) {
            return fail_request(Http3ErrorCode::MessageError);
        }
        frame_payload_remaining_ = header.length;
        body_recv_state_ = BodyRecvState::TrailerPayload;
        return common::IoErr::None;
    }

    if (is_forbidden_request_stream_frame(header.type)) {
        return fail_request(Http3ErrorCode::FrameUnexpected);
    }

    frame_payload_remaining_ = header.length;
    return common::IoErr::None;
}

async::Task<common::IoResult<mem::IoBufChain>>
ServerHttp3Request::read_body(HttpExchange &exchange, std::size_t max_bytes,
                              std::chrono::milliseconds timeout) noexcept {
    mem::IoBufChain out(inbound_buf_.node_pool());
    if (&exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (body_recv_state_ == BodyRecvState::Error) {
        co_return std::unexpected(common::IoErr::Canceled);
    }
    if (max_bytes == 0) {
        if (body_recv_state_ == BodyRecvState::Complete) {
            out.mark_complete();
        }
        co_return out;
    }
    if (body_recv_state_ == BodyRecvState::Complete) {
        out.mark_complete();
        co_return out;
    }

    for (;;) {
        switch (body_recv_state_) {
            case BodyRecvState::FrameHeader: {
                Http3ParseStatus status = parse_frame_header_once();
                if (status == Http3ParseStatus::NeedMore) {
                    if (inbound_buf_.complete()) {
                        if (frame_header_in_progress_) {
                            co_return fail_read_body(Http3ErrorCode::RequestIncomplete);
                        }
                        exchange_.request_trailers_complete_ = true;
                        body_recv_state_ = BodyRecvState::Complete;
                        out.mark_complete();
                        co_return out;
                    }
                    auto read = co_await read_more_input(timeout);
                    if (!read) {
                        co_return std::unexpected(read.error());
                    }
                    continue;
                }
                if (status == Http3ParseStatus::Error) {
                    co_return fail_read_body(frame_parser_.error().h3_error, frame_parser_.error().io_error);
                }

                common::IoErr err = begin_body_frame(current_frame_);
                if (err != common::IoErr::None) {
                    co_return fail_read_body(request_parse_error_, err);
                }
                if (body_recv_state_ == BodyRecvState::FrameHeader) {
                    auto skipped = co_await skip_frame_payload(frame_payload_remaining_, timeout);
                    if (!skipped) {
                        if (request_parse_error_ == Http3ErrorCode::GeneralProtocolError) {
                            co_return std::unexpected(skipped.error());
                        }
                        co_return fail_read_body(request_parse_error_, skipped.error());
                    }
                }
                break;
            }

            case BodyRecvState::DataPayload: {
                if (frame_payload_remaining_ == 0) {
                    body_recv_state_ = BodyRecvState::FrameHeader;
                    continue;
                }

                const std::size_t readable = inbound_buf_.readable_bytes();
                if (readable == 0) {
                    if (inbound_buf_.complete()) {
                        co_return fail_read_body(Http3ErrorCode::RequestIncomplete);
                    }
                    auto read = co_await read_more_input(timeout);
                    if (!read) {
                        co_return std::unexpected(read.error());
                    }
                    continue;
                }

                const std::size_t take =
                        std::min<std::size_t>(max_bytes, std::min<std::uint64_t>(readable, frame_payload_remaining_));
                auto taken = take_body_payload(out, take);
                if (!taken) {
                    co_return std::unexpected(taken.error());
                }
                frame_payload_remaining_ -= take;
                // take_prefix transfers the QUIC FIN only when this read consumed all buffered input.
                // It completes the HTTP body with this DATA chunk only when the DATA frame also ended.
                if (out.complete()) {
                    if (frame_payload_remaining_ != 0) {
                        co_return fail_read_body(Http3ErrorCode::RequestIncomplete);
                    }
                    exchange_.request_trailers_complete_ = true;
                    body_recv_state_ = BodyRecvState::Complete;
                    co_return out;
                }
                if (frame_payload_remaining_ == 0) {
                    body_recv_state_ = BodyRecvState::FrameHeader;
                }
                if (out.readable_bytes() != 0) {
                    co_return out;
                }
                break;
            }

            case BodyRecvState::TrailerPayload: {
                auto parsed = co_await parse_header_block(HeaderBlockTarget::Trailer, frame_payload_remaining_);
                if (!parsed) {
                    if (request_parse_error_ == Http3ErrorCode::GeneralProtocolError) {
                        co_return std::unexpected(parsed.error());
                    }
                    co_return fail_read_body(request_parse_error_, parsed.error());
                }
                frame_payload_remaining_ = 0;
                body_recv_state_ = BodyRecvState::WaitFin;
                break;
            }

            case BodyRecvState::WaitFin: {
                if (inbound_buf_.readable_bytes() == 0 && inbound_buf_.complete() && !frame_header_in_progress_) {
                    body_recv_state_ = BodyRecvState::Complete;
                    out.mark_complete();
                    co_return out;
                }

                Http3ParseStatus status = parse_frame_header_once();
                if (status == Http3ParseStatus::NeedMore) {
                    if (inbound_buf_.complete()) {
                        if (frame_header_in_progress_) {
                            co_return fail_read_body(Http3ErrorCode::RequestIncomplete);
                        }
                        body_recv_state_ = BodyRecvState::Complete;
                        out.mark_complete();
                        co_return out;
                    }
                    auto read = co_await read_more_input(timeout);
                    if (!read) {
                        co_return std::unexpected(read.error());
                    }
                    continue;
                }
                if (status == Http3ParseStatus::Error) {
                    co_return fail_read_body(frame_parser_.error().h3_error, frame_parser_.error().io_error);
                }

                const Http3FrameHeader header = current_frame_;
                if (header.type == static_cast<std::uint64_t>(Http3FrameType::Data) ||
                    header.type == static_cast<std::uint64_t>(Http3FrameType::Headers)) {
                    co_return fail_read_body(Http3ErrorCode::MessageError);
                }
                if (is_forbidden_request_stream_frame(header.type)) {
                    co_return fail_read_body(Http3ErrorCode::FrameUnexpected);
                }
                auto skipped = co_await skip_frame_payload(header.length, timeout);
                if (!skipped) {
                    if (request_parse_error_ == Http3ErrorCode::GeneralProtocolError) {
                        co_return std::unexpected(skipped.error());
                    }
                    co_return fail_read_body(request_parse_error_, skipped.error());
                }
                break;
            }

            case BodyRecvState::Complete:
                out.mark_complete();
                co_return out;
            case BodyRecvState::Error:
                co_return std::unexpected(common::IoErr::Canceled);
        }
    }
}

async::Task<common::IoResult<void>> ServerHttp3Request::send_header(HttpExchange &exchange,
                                                                    const OutgoingHeaderBlockView &header,
                                                                    std::chrono::milliseconds timeout) {
    if (&exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!handler_started_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    HttpBodySpec final_body_spec = HttpBodySpec::Auto();
    switch (header.kind) {
        case OutgoingHeaderKind::Informational:
            if (response_headers_sent_ || header.end_stream || header.status_code < 100 || header.status_code >= 200) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            break;
        case OutgoingHeaderKind::Final:
            if (response_headers_sent_) {
                co_return std::unexpected(common::IoErr::Already);
            }
            if (header.status_code < 200 || header.status_code > 999) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            final_body_spec = header.body;
            if (final_body_spec.is_chunked()) {
                co_return std::unexpected(common::IoErr::NotSupported);
            }
            if (response_must_not_have_body(exchange, header.status_code)) {
                if (!header.end_stream || final_body_spec.is_stream() ||
                    (final_body_spec.is_content_length() && final_body_spec.content_length() != 0)) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }
                final_body_spec = HttpBodySpec::None();
            } else {
                if (final_body_spec.is_none()) {
                    co_return std::unexpected(common::IoErr::Invalid);
                }
                if (header.end_stream) {
                    if (final_body_spec.is_content_length() && final_body_spec.content_length() != 0) {
                        co_return std::unexpected(common::IoErr::Invalid);
                    }
                    if (final_body_spec.is_stream()) {
                        final_body_spec = HttpBodySpec::Auto();
                    }
                }
            }
            break;
        case OutgoingHeaderKind::Trailer:
            if (!response_headers_sent_ || !header.end_stream) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            if (response_body_spec_.is_content_length() && response_body_sent_ != response_content_length_) {
                co_return std::unexpected(common::IoErr::Invalid);
            }
            break;
    }

    Http3QpackEncoderIoBufWriter writer(inbound_buf_.node_pool(),
                                        Http3QpackEncoder::Options{.max_string_size = max_qpack_string_size_}, 512,
                                        kHttp3FrameHeaderReserve);

    if (header.kind != OutgoingHeaderKind::Trailer) {
        common::IoErr err = writer.encode_status(header.status_code);
        if (err != common::IoErr::None) {
            writer.abort();
            co_return std::unexpected(err);
        }
    }

    if (header.headers != nullptr) {
        for (auto it = header.headers->begin(); it != header.headers->end(); ++it) {
            const auto &field = *it;
            if (field.name_len == 0) {
                continue;
            }
            std::string_view name = field.lowcase_view();
            if (name.empty()) {
                name = field.name_view();
            }
            common::IoErr err = writer.encode_field(name, field.name_hash, field.value_view());
            if (err != common::IoErr::None) {
                writer.abort();
                co_return std::unexpected(err);
            }
        }
    }

    mem::IoBufChain frame(inbound_buf_.node_pool());
    auto finished_frame = http3_finish_headers_frame(writer, frame, header.end_stream);
    if (!finished_frame) {
        co_return std::unexpected(finished_frame.error());
    }

    while (!frame.empty()) {
        auto written = co_await stream_.write(frame, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (*written == 0 && !frame.empty()) {
            co_return std::unexpected(common::IoErr::WouldBlock);
        }
    }

    if (header.kind == OutgoingHeaderKind::Final) {
        response_headers_sent_ = true;
        response_body_spec_ = final_body_spec;
        response_content_length_ = final_body_spec.is_content_length() ? final_body_spec.content_length() : 0;
        response_body_sent_ = 0;
    }
    if (header.end_stream) {
        response_finished_ = true;
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write_all(HttpExchange &exchange, mem::IoBufChain chunk,
                                                                         std::chrono::milliseconds timeout) noexcept {
    if (&exchange != &exchange_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (!handler_started_ || !response_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::size_t body_len = chunk.readable_bytes();
    const bool end_stream = chunk.complete();
    if (static_cast<std::uint64_t>(body_len) > kMaxHttp3FramePayloadLength) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    if (response_body_spec_.is_none()) {
        if (body_len != 0 || end_stream) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return body_len;
    }

    if (response_body_spec_.is_content_length()) {
        if (response_body_sent_ > response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::size_t remaining = response_content_length_ - response_body_sent_;
        if (body_len > remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        if (end_stream && body_len != remaining) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    if (response_data_frame_active_) {
        if (body_len != response_data_frame_remaining_ || end_stream != response_data_frame_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        std::size_t total = 0;
        while (chunk.readable_bytes() != 0 || chunk.complete()) {
            auto written = co_await write(exchange, chunk, timeout);
            if (!written) {
                co_return std::unexpected(written.error());
            }
            total += *written;
        }
        co_return total;
    }

    if (body_len == 0 && !end_stream) {
        co_return body_len;
    }

    auto prepared_frame = http3_prepare_data_frame(chunk, inbound_buf_.node_pool());
    if (!prepared_frame) {
        co_return std::unexpected(prepared_frame.error());
    }

    while (chunk.readable_bytes() != 0 || chunk.complete()) {
        auto written = co_await stream_.write(chunk, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (*written == 0 && (chunk.readable_bytes() != 0 || chunk.complete())) {
            co_return std::unexpected(common::IoErr::WouldBlock);
        }
    }

    response_body_sent_ += body_len;
    if (end_stream) {
        response_finished_ = true;
    }
    co_return body_len;
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write_all(HttpExchange &exchange,
                                                                         const std::uint8_t *buf, std::size_t len,
                                                                         bool end,
                                                                         std::chrono::milliseconds timeout) noexcept {
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }

    mem::IoBufChain chunk(inbound_buf_.node_pool());
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

async::Task<common::IoResult<void>>
ServerHttp3Request::write_data_frame_header(std::size_t payload_len, std::chrono::milliseconds timeout) noexcept {
    auto header = http3_build_data_frame_header(payload_len);
    if (!header) {
        co_return std::unexpected(header.error());
    }
    if (!*header) {
        co_return common::IoResult<void>{};
    }

    mem::IoBufChain chain(inbound_buf_.node_pool());
    if (!chain.append(std::move(*header))) {
        co_return std::unexpected(common::IoErr::NoMem);
    }
    while (!chain.empty()) {
        auto written = co_await stream_.write(chain, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (*written == 0) {
            co_return std::unexpected(common::IoErr::WouldBlock);
        }
    }
    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write(HttpExchange &exchange, mem::IoBufChain &chunk,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if (&exchange != &exchange_ || !handler_started_ || !response_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }

    const std::size_t body_len = chunk.readable_bytes();
    const bool end_stream = chunk.complete();
    if (static_cast<std::uint64_t>(body_len) > kMaxHttp3FramePayloadLength) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_body_spec_.is_none()) {
        if (body_len != 0 || end_stream) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return 0;
    }
    if (response_body_spec_.is_content_length()) {
        if (response_body_sent_ > response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::size_t remaining = response_content_length_ - response_body_sent_;
        if (body_len > remaining || (end_stream && body_len != remaining)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    if (response_data_frame_active_) {
        if (body_len != response_data_frame_remaining_ || end_stream != response_data_frame_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        if (body_len == 0) {
            if (!end_stream) {
                co_return 0;
            }
            auto written = co_await stream_.write(nullptr, 0, true, timeout);
            if (!written) {
                co_return std::unexpected(written.error());
            }
            chunk.clear_complete();
            response_finished_ = true;
            co_return 0;
        }
        auto header = co_await write_data_frame_header(body_len, timeout);
        if (!header) {
            co_return std::unexpected(header.error());
        }
        response_data_frame_active_ = true;
        response_data_frame_remaining_ = body_len;
        response_data_frame_end_ = end_stream;
    }

    std::size_t accepted = 0;
    if (chunk.bound() && &chunk.node_pool() == &inbound_buf_.node_pool()) {
        auto written = co_await stream_.write(chunk, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        accepted = *written;
    } else {
        mem::IoBufChain staged(inbound_buf_.node_pool());
        if (!chunk.retain_prefix(body_len, staged)) {
            co_return std::unexpected(common::IoErr::NoMem);
        }
        auto written = co_await stream_.write(staged, timeout);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        accepted = *written;
        chunk.consume_and_compact(accepted);
        if (accepted == body_len && end_stream) {
            chunk.clear_complete();
        }
    }
    if (accepted == 0) {
        co_return std::unexpected(common::IoErr::WouldBlock);
    }

    response_data_frame_remaining_ -= accepted;
    response_body_sent_ += accepted;
    if (response_data_frame_remaining_ == 0) {
        response_data_frame_active_ = false;
        response_data_frame_end_ = false;
        if (end_stream) {
            chunk.clear_complete();
            response_finished_ = true;
        }
    }
    co_return accepted;
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write(HttpExchange &exchange, const std::uint8_t *buf,
                                                                     std::size_t len, bool end,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if ((len != 0 && buf == nullptr) || &exchange != &exchange_ || !handler_started_ || !response_headers_sent_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_finished_) {
        co_return std::unexpected(common::IoErr::Already);
    }
    if (static_cast<std::uint64_t>(len) > kMaxHttp3FramePayloadLength) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (response_body_spec_.is_none()) {
        if (len != 0 || end) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        co_return 0;
    }
    if (response_body_spec_.is_content_length()) {
        if (response_body_sent_ > response_content_length_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
        const std::size_t remaining = response_content_length_ - response_body_sent_;
        if (len > remaining || (end && len != remaining)) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    }

    if (response_data_frame_active_) {
        if (len != response_data_frame_remaining_ || end != response_data_frame_end_) {
            co_return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        if (len == 0) {
            if (!end) {
                co_return 0;
            }
            auto written = co_await stream_.write(nullptr, 0, true, timeout);
            if (!written) {
                co_return std::unexpected(written.error());
            }
            response_finished_ = true;
            co_return 0;
        }
        auto header = co_await write_data_frame_header(len, timeout);
        if (!header) {
            co_return std::unexpected(header.error());
        }
        response_data_frame_active_ = true;
        response_data_frame_remaining_ = len;
        response_data_frame_end_ = end;
    }

    auto written = co_await stream_.write(buf, len, end, timeout);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    if (*written == 0) {
        co_return std::unexpected(common::IoErr::WouldBlock);
    }
    response_data_frame_remaining_ -= *written;
    response_body_sent_ += *written;
    if (response_data_frame_remaining_ == 0) {
        response_data_frame_active_ = false;
        response_data_frame_end_ = false;
        if (end) {
            response_finished_ = true;
        }
    }
    co_return *written;
}

common::IoResult<void> ServerHttp3Request::abort(HttpExchange &exchange, common::IoErr) noexcept {
    if (&exchange != &exchange_ || !handler_started_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    stream_.close(error_value(Http3ErrorCode::RequestCancelled));
    response_finished_ = true;
    return {};
}

} // namespace fiber::http
