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
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
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
    } else {
        request->reading_trailers_ = true;
        request->exchange_.request_trailers_complete_ = true;
    }
    if (end_stream && !request->reading_trailers_) {
        request->exchange_.request_trailers_complete_ = true;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_body(void *, mem::IoBuf &&, bool) noexcept { return common::IoErr::None; }

void ServerHttp2Request::destroy_owner(void *owner) noexcept { delete static_cast<ServerHttp2Request *>(owner); }

common::IoErr ServerHttp2Request::on_indexed_field(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    if (is_pseudo_header(entry.name)) {
        if (request->reading_trailers_ || request->saw_regular_header_in_block_) {
            return common::IoErr::Invalid;
        }

        if (entry.name == ":method") {
            std::string_view method = request->copy_to_pool(entry.value);
            if (method.data() == nullptr && !method.empty()) {
                return common::IoErr::NoMem;
            }
            request->exchange_.method_view_ = method;
            request->exchange_.method_ = parse_method(method);
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            return common::IoErr::None;
        }

        if (entry.name == ":path") {
            std::string_view path = request->copy_to_pool(entry.value);
            if (path.data() == nullptr && !path.empty()) {
                return common::IoErr::NoMem;
            }
            request->exchange_.uri_.unparsed_uri = path;
            request->exchange_.uri_.path = path;
            request->exchange_.uri_.query = {};
            request->exchange_.uri_.exten = {};
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '?') {
                    request->exchange_.uri_.path = path.substr(0, i);
                    request->exchange_.uri_.query = path.substr(i + 1);
                    break;
                }
            }
            std::size_t slash = request->exchange_.uri_.path.find_last_of('/');
            std::size_t dot = request->exchange_.uri_.path.find_last_of('.');
            if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash + 1)) {
                request->exchange_.uri_.exten = request->exchange_.uri_.path.substr(dot + 1);
            }
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            return common::IoErr::None;
        }

        if (entry.name == ":authority") {
            std::string_view name_copy = request->copy_to_pool("host");
            std::string_view value_copy = request->copy_to_pool(entry.value);
            if ((!name_copy.data()) || (!value_copy.data() && !entry.value.empty())) {
                return common::IoErr::NoMem;
            }

            HttpHeaders &target =
                request->reading_trailers_ ? request->exchange_.request_trailers_ : request->exchange_.request_headers_;
            char *lowcase_name = const_cast<char *>(name_copy.data());
            if (!target.add_view(name_copy, value_copy, lowcase_name, http_header_name_hash("host"))) {
                return common::IoErr::NoMem;
            }
            return common::IoErr::None;
        }

        if (entry.name == ":scheme") {
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            return common::IoErr::None;
        }

        return common::IoErr::Invalid;
    }

    request->saw_regular_header_in_block_ = true;
    std::string_view name_copy = request->copy_to_pool(entry.name);
    std::string_view value_copy = request->copy_to_pool(entry.value);
    if ((!name_copy.data() && !entry.name.empty()) || (!value_copy.data() && !entry.value.empty())) {
        return common::IoErr::NoMem;
    }

    HttpHeaders &target = request->reading_trailers_ ? request->exchange_.request_trailers_ : request->exchange_.request_headers_;
    char *lowcase_name = name_copy.empty() ? nullptr : const_cast<char *>(name_copy.data());
    if (!target.add_view(name_copy, value_copy, lowcase_name, entry.name_hash)) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_indexed_name(void *owner, std::string_view name,
                                                  std::uint64_t name_hash) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    request->pending_name_ = name;
    request->pending_name_hash_ = name_hash;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_name_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                              Http2HpackDecoder::NameView &out) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    common::IoErr err = request->materialize_name_raw(data, len, out);
    if (err != common::IoErr::None) {
        return err;
    }
    request->pending_name_ = out.name;
    request->pending_name_hash_ = out.name_hash;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                  Http2HpackDecoder::NameView &out) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    common::IoErr err = request->materialize_name_huffman(data, len, out);
    if (err != common::IoErr::None) {
        return err;
    }
    request->pending_name_ = out.name;
    request->pending_name_hash_ = out.name_hash;
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                               std::string_view &out) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    common::IoErr err = request->materialize_value_raw(data, len, out);
    if (err != common::IoErr::None) {
        return err;
    }
    if (request->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    if (is_pseudo_header(request->pending_name_)) {
        if (request->reading_trailers_ || request->saw_regular_header_in_block_) {
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::Invalid;
        }

        if (request->pending_name_ == ":method") {
            std::string_view method = request->copy_to_pool(out);
            if (method.data() == nullptr && !method.empty()) {
                request->pending_name_ = {};
                request->pending_name_hash_ = 0;
                return common::IoErr::NoMem;
            }
            request->exchange_.method_view_ = method;
            request->exchange_.method_ = parse_method(method);
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::None;
        }

        if (request->pending_name_ == ":path") {
            std::string_view path = request->copy_to_pool(out);
            if (path.data() == nullptr && !path.empty()) {
                request->pending_name_ = {};
                request->pending_name_hash_ = 0;
                return common::IoErr::NoMem;
            }
            request->exchange_.uri_.unparsed_uri = path;
            request->exchange_.uri_.path = path;
            request->exchange_.uri_.query = {};
            request->exchange_.uri_.exten = {};
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '?') {
                    request->exchange_.uri_.path = path.substr(0, i);
                    request->exchange_.uri_.query = path.substr(i + 1);
                    break;
                }
            }
            std::size_t slash = request->exchange_.uri_.path.find_last_of('/');
            std::size_t dot = request->exchange_.uri_.path.find_last_of('.');
            if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash + 1)) {
                request->exchange_.uri_.exten = request->exchange_.uri_.path.substr(dot + 1);
            }
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::None;
        }

        if (request->pending_name_ == ":authority") {
            std::string_view name_copy = request->copy_to_pool("host");
            std::string_view value_copy = request->copy_to_pool(out);
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            if ((!name_copy.data()) || (!value_copy.data() && !out.empty())) {
                return common::IoErr::NoMem;
            }

            HttpHeaders &target =
                request->reading_trailers_ ? request->exchange_.request_trailers_ : request->exchange_.request_headers_;
            char *lowcase_name = const_cast<char *>(name_copy.data());
            if (!target.add_view(name_copy, value_copy, lowcase_name, http_header_name_hash("host"))) {
                return common::IoErr::NoMem;
            }
            return common::IoErr::None;
        }

        if (request->pending_name_ == ":scheme") {
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::None;
        }

        request->pending_name_ = {};
        request->pending_name_hash_ = 0;
        return common::IoErr::Invalid;
    }

    request->saw_regular_header_in_block_ = true;
    std::uint64_t pending_name_hash = request->pending_name_hash_;
    std::string_view pending_name = request->pending_name_;
    std::string_view name_copy = request->copy_to_pool(pending_name);
    std::string_view value_copy = request->copy_to_pool(out);
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    if ((!name_copy.data() && !pending_name.empty()) || (!value_copy.data() && !out.empty())) {
        return common::IoErr::NoMem;
    }

    HttpHeaders &target = request->reading_trailers_ ? request->exchange_.request_trailers_ : request->exchange_.request_headers_;
    char *lowcase_name = name_copy.empty() ? nullptr : const_cast<char *>(name_copy.data());
    if (!target.add_view(name_copy, value_copy, lowcase_name, pending_name_hash)) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                   std::string_view &out) noexcept {
    auto *request = static_cast<ServerHttp2Request *>(owner);
    common::IoErr err = request->materialize_value_huffman(data, len, out);
    if (err != common::IoErr::None) {
        return err;
    }
    if (request->pending_name_.data() == nullptr) {
        return common::IoErr::Invalid;
    }
    if (is_pseudo_header(request->pending_name_)) {
        if (request->reading_trailers_ || request->saw_regular_header_in_block_) {
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::Invalid;
        }

        if (request->pending_name_ == ":method") {
            std::string_view method = request->copy_to_pool(out);
            if (method.data() == nullptr && !method.empty()) {
                request->pending_name_ = {};
                request->pending_name_hash_ = 0;
                return common::IoErr::NoMem;
            }
            request->exchange_.method_view_ = method;
            request->exchange_.method_ = parse_method(method);
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::None;
        }

        if (request->pending_name_ == ":path") {
            std::string_view path = request->copy_to_pool(out);
            if (path.data() == nullptr && !path.empty()) {
                request->pending_name_ = {};
                request->pending_name_hash_ = 0;
                return common::IoErr::NoMem;
            }
            request->exchange_.uri_.unparsed_uri = path;
            request->exchange_.uri_.path = path;
            request->exchange_.uri_.query = {};
            request->exchange_.uri_.exten = {};
            for (std::size_t i = 0; i < path.size(); ++i) {
                if (path[i] == '?') {
                    request->exchange_.uri_.path = path.substr(0, i);
                    request->exchange_.uri_.query = path.substr(i + 1);
                    break;
                }
            }
            std::size_t slash = request->exchange_.uri_.path.find_last_of('/');
            std::size_t dot = request->exchange_.uri_.path.find_last_of('.');
            if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash + 1)) {
                request->exchange_.uri_.exten = request->exchange_.uri_.path.substr(dot + 1);
            }
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::None;
        }

        if (request->pending_name_ == ":authority") {
            std::string_view name_copy = request->copy_to_pool("host");
            std::string_view value_copy = request->copy_to_pool(out);
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            if ((!name_copy.data()) || (!value_copy.data() && !out.empty())) {
                return common::IoErr::NoMem;
            }

            HttpHeaders &target =
                request->reading_trailers_ ? request->exchange_.request_trailers_ : request->exchange_.request_headers_;
            char *lowcase_name = const_cast<char *>(name_copy.data());
            if (!target.add_view(name_copy, value_copy, lowcase_name, http_header_name_hash("host"))) {
                return common::IoErr::NoMem;
            }
            return common::IoErr::None;
        }

        if (request->pending_name_ == ":scheme") {
            request->exchange_.version_ = HttpVersion::HTTP_2_0;
            request->pending_name_ = {};
            request->pending_name_hash_ = 0;
            return common::IoErr::None;
        }

        request->pending_name_ = {};
        request->pending_name_hash_ = 0;
        return common::IoErr::Invalid;
    }

    request->saw_regular_header_in_block_ = true;
    std::string_view name_copy = request->copy_to_pool(request->pending_name_);
    std::string_view value_copy = request->copy_to_pool(out);
    std::uint64_t pending_name_hash = request->pending_name_hash_;
    std::string_view pending_name = request->pending_name_;
    request->pending_name_ = {};
    request->pending_name_hash_ = 0;
    if ((!name_copy.data() && !pending_name.empty()) || (!value_copy.data() && !out.empty())) {
        return common::IoErr::NoMem;
    }

    HttpHeaders &target = request->reading_trailers_ ? request->exchange_.request_trailers_ : request->exchange_.request_headers_;
    char *lowcase_name = name_copy.empty() ? nullptr : const_cast<char *>(name_copy.data());
    if (!target.add_view(name_copy, value_copy, lowcase_name, pending_name_hash)) {
        return common::IoErr::NoMem;
    }
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::materialize_name_raw(const std::uint8_t *data, std::size_t len,
                                                       Http2HpackDecoder::NameView &out) noexcept {
    std::string_view name = copy_to_pool(data, len);
    if (!name.data() && len != 0) {
        return common::IoErr::NoMem;
    }
    out.name = name;
    out.name_hash = http_header_name_hash(name);
    return common::IoErr::None;
}

common::IoErr ServerHttp2Request::materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                           Http2HpackDecoder::NameView &out) noexcept {
    out.name = {};
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

        out.name = std::string_view(mem, decoded_len);
    }
    out.name_hash = http_header_name_hash(out.name);
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
