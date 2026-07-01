#include "ServerHttp3Request.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>
#include <new>
#include <utility>

#include "../async/Spawn.h"
#include "../event/EventLoop.h"
#include "Http3Codec.h"
#include "Http3Connection.h"
#include "Http3QpackDecoder.h"
#include "HttpHeaderHash.h"
#include "HttpUriParse.h"
#include "Huffman.h"

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

bool is_forbidden_request_stream_frame(std::uint64_t type) noexcept {
    switch (static_cast<Http3FrameType>(type)) {
        case Http3FrameType::Data:
        case Http3FrameType::CancelPush:
        case Http3FrameType::Settings:
        case Http3FrameType::PushPromise:
        case Http3FrameType::Goaway:
        case Http3FrameType::MaxPushId:
            return true;
        case Http3FrameType::Headers:
            return false;
    }
    return false;
}

} // namespace

ServerHttp3Request::ServerHttp3Request(Http3Connection &conn, const HttpServerOptions &http_options,
                                       const HttpHandler &handler) noexcept :
    quic_lease_(conn.quic().lease()), stream_(this, &ServerHttp3Request::destroy_owner),
    inbound_buf_(conn.quic().recv_extent_pool()), exchange_(conn.quic().recv_extent_pool(), http_options),
    handler_(&handler), max_qpack_string_size_(static_cast<std::uint32_t>(std::min<std::size_t>(
                                http_options.header_large_size, std::numeric_limits<std::uint32_t>::max()))) {}

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

class ServerHttp3Request::RequestHeaderParser final : public common::NonCopyable, public common::NonMovable {
public:
    explicit RequestHeaderParser(ServerHttp3Request &request) noexcept : request_(request) {}

    [[nodiscard]] bool init() noexcept { return qpack_decoder_.init(request_.max_qpack_string_size_); }
    [[nodiscard]] common::IoErr process_available_input() noexcept;

private:
    enum class ParseState : std::uint8_t {
        FrameHeader,
        HeaderPayload,
        SkipPayload,
    };

    [[nodiscard]] static const Http3QpackDecoder::Ops &decoder_ops() noexcept;
    static common::IoErr on_indexed_field(void *owner, Http3QpackDecoder::TableEntryView entry) noexcept;
    static common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept;
    static common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;

    [[nodiscard]] common::IoErr begin_frame_payload(const Http3FrameHeader &header) noexcept;
    [[nodiscard]] common::IoErr decode_header_payload() noexcept;
    [[nodiscard]] common::IoErr complete_request_header_block() noexcept;
    [[nodiscard]] common::IoErr fail(Http3ErrorCode error, common::IoErr reason = common::IoErr::Invalid) noexcept;
    [[nodiscard]] common::IoErr materialize_name_raw(const std::uint8_t *data, std::size_t len, std::string_view &out,
                                                     std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                         std::string_view &out, std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                      std::string_view &out) noexcept;
    [[nodiscard]] common::IoErr materialize_value_huffman(const std::uint8_t *data, std::size_t len,
                                                          std::string_view &out) noexcept;
    [[nodiscard]] common::IoErr commit_field(std::string_view name, std::uint64_t name_hash, std::string_view value,
                                             bool name_owned = false) noexcept;
    [[nodiscard]] common::IoErr commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                      std::string_view value, bool name_owned = false) noexcept;
    [[nodiscard]] common::IoErr handle_pseudo_header(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_method(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_path(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_scheme(std::string_view value) noexcept;
    [[nodiscard]] common::IoErr handle_authority(std::string_view value) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(const std::uint8_t *data, std::size_t len) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(std::string_view value) noexcept;

    ServerHttp3Request &request_;
    Http3FrameHeaderParser frame_parser_;
    Http3PayloadSkipParser skip_parser_;
    Http3QpackDecoder qpack_decoder_;
    ParseState parse_state_ = ParseState::FrameHeader;
    std::uint64_t frame_payload_remaining_ = 0;
    std::string_view pending_name_;
    std::uint64_t pending_name_hash_ = 0;
    bool pending_name_owned_ = false;
    bool saw_regular_header_in_block_ = false;
};

const Http3QpackDecoder::Ops &ServerHttp3Request::RequestHeaderParser::decoder_ops() noexcept {
    static const Http3QpackDecoder::Ops kOps{
            &RequestHeaderParser::on_indexed_field, &RequestHeaderParser::on_indexed_name,
            &RequestHeaderParser::on_name_raw,      &RequestHeaderParser::on_name_huffman,
            &RequestHeaderParser::on_value_raw,     &RequestHeaderParser::on_value_huffman,
    };
    return kOps;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::process_available_input() noexcept {
    for (;;) {
        switch (parse_state_) {
            case ParseState::FrameHeader: {
                Http3ParseStatus status = frame_parser_.parse(request_.inbound_buf_);
                if (status == Http3ParseStatus::NeedMore) {
                    return common::IoErr::None;
                }
                if (status == Http3ParseStatus::Error) {
                    return fail(frame_parser_.error().h3_error, frame_parser_.error().io_error);
                }
                Http3FrameHeader header = frame_parser_.header();
                frame_parser_.reset();
                common::IoErr err = begin_frame_payload(header);
                if (err != common::IoErr::None || request_.request_head_received_) {
                    return err;
                }
                break;
            }
            case ParseState::HeaderPayload: {
                common::IoErr err = decode_header_payload();
                if (err != common::IoErr::None || request_.request_head_received_ ||
                    request_.inbound_buf_.readable_bytes() == 0) {
                    return err;
                }
                break;
            }
            case ParseState::SkipPayload: {
                Http3ParseStatus status = skip_parser_.parse(request_.inbound_buf_);
                if (status == Http3ParseStatus::NeedMore) {
                    return common::IoErr::None;
                }
                if (status == Http3ParseStatus::Error) {
                    return fail(skip_parser_.error().h3_error, skip_parser_.error().io_error);
                }
                skip_parser_.reset();
                parse_state_ = ParseState::FrameHeader;
                break;
            }
        }
    }
}

common::IoErr ServerHttp3Request::RequestHeaderParser::begin_frame_payload(const Http3FrameHeader &header) noexcept {
    if (header.type == static_cast<std::uint64_t>(Http3FrameType::Headers)) {
        pending_name_ = {};
        pending_name_hash_ = 0;
        pending_name_owned_ = false;
        saw_regular_header_in_block_ = false;
        frame_payload_remaining_ = header.length;
        qpack_decoder_.begin_block(this, &decoder_ops());
        parse_state_ = ParseState::HeaderPayload;
        return decode_header_payload();
    }

    if (is_forbidden_request_stream_frame(header.type)) {
        return fail(Http3ErrorCode::FrameUnexpected);
    }

    skip_parser_.start(header.length);
    parse_state_ = ParseState::SkipPayload;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::decode_header_payload() noexcept {
    if (frame_payload_remaining_ == 0) {
        common::IoErr err = qpack_decoder_.decode(nullptr, 0, true);
        if (err != common::IoErr::None) {
            return fail(Http3ErrorCode::QpackDecompressionFailed, err);
        }
        return complete_request_header_block();
    }

    while (frame_payload_remaining_ != 0) {
        mem::IoBuf *buf = request_.inbound_buf_.first_readable();
        if (buf == nullptr || buf->readable() == 0) {
            return common::IoErr::None;
        }

        const std::size_t take = std::min<std::uint64_t>(buf->readable(), frame_payload_remaining_);
        const bool end_block = static_cast<std::uint64_t>(take) == frame_payload_remaining_;
        common::IoErr err = qpack_decoder_.decode(buf->readable_data(), take, end_block);
        if (err != common::IoErr::None) {
            return fail(Http3ErrorCode::QpackDecompressionFailed, err);
        }
        request_.inbound_buf_.consume_and_compact(take);
        frame_payload_remaining_ -= take;
        if (end_block) {
            return complete_request_header_block();
        }
    }

    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::complete_request_header_block() noexcept {
    if (pending_name_.data() != nullptr || request_.request_head_received_) {
        return fail(Http3ErrorCode::MessageError);
    }
    request_.request_head_received_ = true;
    parse_state_ = ParseState::FrameHeader;
    frame_payload_remaining_ = 0;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::fail(Http3ErrorCode error, common::IoErr reason) noexcept {
    request_.header_parse_error_ = error;
    return reason == common::IoErr::None ? common::IoErr::Invalid : reason;
}

common::IoErr
ServerHttp3Request::RequestHeaderParser::on_indexed_field(void *owner,
                                                          Http3QpackDecoder::TableEntryView entry) noexcept {
    auto *parser = static_cast<RequestHeaderParser *>(owner);
    return parser->commit_field(entry.name, entry.name_hash, entry.value);
}

common::IoErr ServerHttp3Request::RequestHeaderParser::on_indexed_name(void *owner, std::string_view name,
                                                                       std::uint64_t name_hash) noexcept {
    auto *parser = static_cast<RequestHeaderParser *>(owner);
    parser->pending_name_ = name;
    parser->pending_name_hash_ = name_hash;
    parser->pending_name_owned_ = false;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::on_name_raw(void *owner, const std::uint8_t *data,
                                                                   std::size_t len) noexcept {
    auto *parser = static_cast<RequestHeaderParser *>(owner);
    std::string_view name;
    std::uint64_t name_hash = 0;
    common::IoErr err = parser->materialize_name_raw(data, len, name, name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    parser->pending_name_ = name;
    parser->pending_name_hash_ = name_hash;
    parser->pending_name_owned_ = true;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::on_name_huffman(void *owner, const std::uint8_t *data,
                                                                       std::size_t len) noexcept {
    auto *parser = static_cast<RequestHeaderParser *>(owner);
    std::string_view name;
    std::uint64_t name_hash = 0;
    common::IoErr err = parser->materialize_name_huffman(data, len, name, name_hash);
    if (err != common::IoErr::None) {
        return err;
    }
    parser->pending_name_ = name;
    parser->pending_name_hash_ = name_hash;
    parser->pending_name_owned_ = true;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::on_value_raw(void *owner, const std::uint8_t *data,
                                                                    std::size_t len) noexcept {
    auto *parser = static_cast<RequestHeaderParser *>(owner);
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
    bool pending_name_owned = parser->pending_name_owned_;
    parser->pending_name_ = {};
    parser->pending_name_hash_ = 0;
    parser->pending_name_owned_ = false;
    return parser->commit_field(pending_name, pending_name_hash, value, pending_name_owned);
}

common::IoErr ServerHttp3Request::RequestHeaderParser::on_value_huffman(void *owner, const std::uint8_t *data,
                                                                        std::size_t len) noexcept {
    auto *parser = static_cast<RequestHeaderParser *>(owner);
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
    bool pending_name_owned = parser->pending_name_owned_;
    parser->pending_name_ = {};
    parser->pending_name_hash_ = 0;
    parser->pending_name_owned_ = false;
    return parser->commit_field(pending_name, pending_name_hash, value, pending_name_owned);
}

common::IoErr ServerHttp3Request::RequestHeaderParser::materialize_name_raw(const std::uint8_t *data, std::size_t len,
                                                                            std::string_view &out,
                                                                            std::uint64_t &name_hash) noexcept {
    out = copy_to_pool(data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    name_hash = http_header_name_hash(out);
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::materialize_name_huffman(const std::uint8_t *data,
                                                                                std::size_t len, std::string_view &out,
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

common::IoErr ServerHttp3Request::RequestHeaderParser::materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                                             std::string_view &out) noexcept {
    out = copy_to_pool(data, len);
    if (!out.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::materialize_value_huffman(const std::uint8_t *data,
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

common::IoErr ServerHttp3Request::RequestHeaderParser::commit_field(std::string_view name, std::uint64_t name_hash,
                                                                    std::string_view value, bool name_owned) noexcept {
    if (is_pseudo_header(name)) {
        if (saw_regular_header_in_block_) {
            return common::IoErr::Invalid;
        }
        return handle_pseudo_header(name, value);
    }

    saw_regular_header_in_block_ = true;
    return commit_regular_header(name, name_hash, value, name_owned);
}

common::IoErr ServerHttp3Request::RequestHeaderParser::commit_regular_header(std::string_view name,
                                                                             std::uint64_t name_hash,
                                                                             std::string_view value,
                                                                             bool name_owned) noexcept {
    std::string_view name_copy = name_owned ? name : copy_to_pool(name);
    std::string_view value_copy = copy_to_pool(value);
    if ((!name_copy.data() && !name.empty()) || (!value_copy.data() && !value.empty())) {
        return common::IoErr::NoMem;
    }

    char *lowcase_name = name_copy.empty() ? nullptr : const_cast<char *>(name_copy.data());
    auto *field = request_.exchange_.request_headers_.add_view(name_copy, value_copy, lowcase_name, name_hash);
    if (!field) {
        return common::IoErr::NoMem;
    }
    request_.exchange_.cache_request_header_field(*field);
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::handle_pseudo_header(std::string_view name,
                                                                            std::string_view value) noexcept {
    if (name == ":method") {
        return handle_method(value);
    }
    if (name == ":path") {
        return handle_path(value);
    }
    if (name == ":scheme") {
        return handle_scheme(value);
    }
    if (name == ":authority") {
        return handle_authority(value);
    }
    return common::IoErr::Invalid;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::handle_method(std::string_view value) noexcept {
    std::string_view method = copy_to_pool(value);
    if (method.data() == nullptr && !method.empty()) {
        return common::IoErr::NoMem;
    }
    request_.exchange_.method_view_ = method;
    request_.exchange_.method_ = parse_method(method);
    request_.exchange_.version_ = HttpVersion::HTTP_3_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::handle_path(std::string_view value) noexcept {
    std::string_view path = copy_to_pool(value);
    if (path.data() == nullptr && !path.empty()) {
        return common::IoErr::NoMem;
    }
    HttpUriParseState uri_state{};
    common::IoErr err = http_parse_uri(path, uri_state);
    if (err != common::IoErr::None) {
        return err;
    }
    err = http_process_uri(path, uri_state, request_.exchange_.uri_, &request_.exchange_.pool());
    if (err != common::IoErr::None) {
        return err;
    }
    request_.exchange_.version_ = HttpVersion::HTTP_3_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::handle_scheme(std::string_view) noexcept {
    request_.exchange_.version_ = HttpVersion::HTTP_3_0;
    return common::IoErr::None;
}

common::IoErr ServerHttp3Request::RequestHeaderParser::handle_authority(std::string_view value) noexcept {
    return commit_regular_header("host", http_header_name_hash("host"), value);
}

std::string_view ServerHttp3Request::RequestHeaderParser::copy_to_pool(const std::uint8_t *data,
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

std::string_view ServerHttp3Request::RequestHeaderParser::copy_to_pool(std::string_view value) noexcept {
    return copy_to_pool(reinterpret_cast<const std::uint8_t *>(value.data()), value.size());
}

async::DetachedTask ServerHttp3Request::run_read_loop(quic::QuicStream::Lease lease) noexcept {
    if (!lease) {
        co_return;
    }

    auto parsed = co_await parse_request_header();
    if (!parsed) {
        stream_.close(error_value(header_parse_error_));
        co_return;
    }

    if (handler_ == nullptr) {
        stream_.close(error_value(Http3ErrorCode::InternalError));
        co_return;
    }

    exchange_.set_io(this);
    handler_started_ = true;
    co_await (*handler_)(exchange_);
    handler_done_ = true;
    exchange_.set_io(nullptr);

    stream_.close(error_value(Http3ErrorCode::RequestCancelled));
    (void) lease;
    co_return;
}

async::Task<common::IoResult<void>> ServerHttp3Request::parse_request_header() noexcept {
    RequestHeaderParser parser(*this);
    if (!parser.init()) {
        header_parse_error_ = Http3ErrorCode::InternalError;
        co_return std::unexpected(common::IoErr::NoMem);
    }

    while (!request_head_received_) {
        common::IoErr err = parser.process_available_input();
        if (err != common::IoErr::None) {
            co_return std::unexpected(err);
        }
        if (request_head_received_) {
            co_return common::IoResult<void>{};
        }
        if (inbound_buf_.complete()) {
            header_parse_error_ = Http3ErrorCode::RequestIncomplete;
            co_return std::unexpected(common::IoErr::Invalid);
        }

        auto read = co_await stream_.read(kHttp3RequestReadChunkSize, inbound_buf_);
        if (!read) {
            header_parse_error_ = Http3ErrorCode::RequestIncomplete;
            co_return std::unexpected(read.error());
        }
    }

    co_return common::IoResult<void>{};
}

async::Task<common::IoResult<mem::IoBufChain>> ServerHttp3Request::read_body(HttpExchange &, std::size_t) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

async::Task<common::IoResult<void>> ServerHttp3Request::send_header(HttpExchange &, const OutgoingHeaderBlockView &) {
    co_return std::unexpected(common::IoErr::NotSupported);
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write_body(HttpExchange &, mem::IoBufChain) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

async::Task<common::IoResult<std::size_t>> ServerHttp3Request::write_body(HttpExchange &, const std::uint8_t *,
                                                                          std::size_t, bool) noexcept {
    co_return std::unexpected(common::IoErr::NotSupported);
}

} // namespace fiber::http
