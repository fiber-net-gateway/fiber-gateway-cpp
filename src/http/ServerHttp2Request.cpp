#include "ServerHttp2Request.h"

#include <cstring>
#include <new>

#include "Http2Connection.h"
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

const Http2Stream::Ops &ServerHttp2Request::stream_ops() noexcept {
    static const Http2Stream::Ops kOps{
        &ServerHttp2Request::destroy_owner,
        &ServerHttp2Request::on_header_block_start,
        &ServerHttp2Request::on_header_block_complete,
        &ServerHttp2Request::on_body,
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
                                       const HttpServerOptions &http_options) noexcept :
    conn_(&conn), stream_(stream_id, this, stream_ops()), exchange_(http_options) {}

Http2Stream::Lease ServerHttp2Request::create(std::uint32_t stream_id, Http2Connection &conn,
                                              const HttpServerOptions &http_options) noexcept {
    auto *owner = new (std::nothrow) ServerHttp2Request(stream_id, conn, http_options);
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
        if (end_stream) {
            request->exchange_.request_trailers_complete_ = true;
        }
        return common::IoErr::None;
    }

    if (!request->reading_trailers_ || !end_stream) {
        return common::IoErr::Invalid;
    }

    request->reading_trailers_ = true;
    request->exchange_.request_trailers_complete_ = true;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_body(void *owner, mem::IoBuf &&, bool end_stream) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (!request->request_head_received_ || request->reading_trailers_ || request->exchange_.request_trailers_complete_) {
        return common::IoErr::Invalid;
    }
    if (end_stream) {
        request->exchange_.request_trailers_complete_ = true;
    }
    return common::IoErr::None;
}

void ServerHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ServerHttp2Request *>(owner); }

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
            http2_huffman_decode(state, data, len, reinterpret_cast<std::uint8_t *>(mem), decoded_len, true);
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
        http2_huffman_decode(state, data, len, reinterpret_cast<std::uint8_t *>(mem), decoded_len, true);
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
    if (!target.add_view(name_copy, value_copy, lowcase_name, name_hash)) {
        return common::IoErr::NoMem;
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
