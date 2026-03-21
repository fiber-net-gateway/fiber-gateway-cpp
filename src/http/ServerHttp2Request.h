#ifndef FIBER_HTTP_SERVER_HTTP2_REQUEST_H
#define FIBER_HTTP_SERVER_HTTP2_REQUEST_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HeaderMap.h"
#include "HttpExchange.h"
#include "HttpHeaderHash.h"
#include "Http2Stream.h"

namespace fiber::http {

class Http2Connection;

class ServerHttp2Request : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static Http2Stream::Lease create(std::uint32_t stream_id, Http2Connection &conn,
                                                   const HttpServerOptions &http_options) noexcept;

    [[nodiscard]] Http2Stream &stream() noexcept { return stream_; }
    [[nodiscard]] const Http2Stream &stream() const noexcept { return stream_; }
    [[nodiscard]] HttpExchange &exchange() noexcept { return exchange_; }
    [[nodiscard]] const HttpExchange &exchange() const noexcept { return exchange_; }

private:
    using PseudoHeaderHandler = common::IoErr (*)(ServerHttp2Request &, std::string_view value) noexcept;

    static const Http2Stream::Ops &stream_ops() noexcept;
    static const Http2HpackDecoder::Ops &decoder_ops() noexcept;
    static const HeaderMap<PseudoHeaderHandler> &pseudo_header_handler_map() noexcept;
    ServerHttp2Request(std::uint32_t stream_id, Http2Connection &conn, const HttpServerOptions &http_options) noexcept;
    static common::IoErr on_header_block_start(void *owner, Http2HpackDecoder::Sink &sink) noexcept;
    static common::IoErr on_header_block_complete(void *owner, bool end_stream) noexcept;
    static common::IoErr on_body(void *owner, mem::IoBuf &&buf, bool end_stream) noexcept;
    static void destroy_owner(void *owner) noexcept;
    static common::IoErr on_indexed_field(void *owner, Http2HpackDecoder::TableEntryView entry) noexcept;
    static common::IoErr on_indexed_name(void *owner, std::string_view name, std::uint64_t name_hash) noexcept;
    static common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept;
    static common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                      Http2HpackDecoder::FieldView *out) noexcept;
    static common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                          Http2HpackDecoder::FieldView *out) noexcept;
    static common::IoErr handle_method(ServerHttp2Request &request, std::string_view value) noexcept;
    static common::IoErr handle_path(ServerHttp2Request &request, std::string_view value) noexcept;
    static common::IoErr handle_scheme(ServerHttp2Request &request, std::string_view value) noexcept;
    static common::IoErr handle_authority(ServerHttp2Request &request, std::string_view value) noexcept;
    [[nodiscard]] common::IoErr materialize_name_raw(const std::uint8_t *data, std::size_t len, std::string_view &out,
                                                     std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_name_huffman(const std::uint8_t *data, std::size_t len,
                                                         std::string_view &out, std::uint64_t &name_hash) noexcept;
    [[nodiscard]] common::IoErr materialize_value_raw(const std::uint8_t *data, std::size_t len,
                                                      std::string_view &out) noexcept;
    [[nodiscard]] common::IoErr materialize_value_huffman(const std::uint8_t *data, std::size_t len,
                                                          std::string_view &out) noexcept;
    [[nodiscard]] common::IoErr commit_field(std::string_view name, std::uint64_t name_hash,
                                             std::string_view value, bool name_owned = false) noexcept;
    [[nodiscard]] common::IoErr commit_regular_header(std::string_view name, std::uint64_t name_hash,
                                                      std::string_view value, bool name_owned = false) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(const std::uint8_t *data, std::size_t len) noexcept;
    [[nodiscard]] std::string_view copy_to_pool(std::string_view value) noexcept;

    [[maybe_unused]] Http2Connection *conn_ = nullptr;
    Http2Stream stream_;
    HttpExchange exchange_;
    bool reading_trailers_ = false;
    bool saw_regular_header_in_block_ = false;
    bool request_head_received_ = false;
    std::string_view pending_name_;
    std::uint64_t pending_name_hash_ = 0;
    bool pending_name_owned_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_HTTP2_REQUEST_H
