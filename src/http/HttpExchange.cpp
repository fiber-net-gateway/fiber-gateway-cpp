#include "HttpExchange.h"

#include <cstring>
#include <limits>
#include <utility>

#include "../common/Assert.h"
#include "HeaderMap.h"
#include "HttpExchangeIo.h"

namespace fiber::http {

namespace {

enum class RequestHeaderRefKind : std::uint8_t {
    Host,
    ContentType,
    Range,
    IfRange,
    Expect,
};

const HeaderMap<RequestHeaderRefKind> &request_header_ref_map() noexcept {
    static HeaderMap<RequestHeaderRefKind> refs = []() {
        HeaderMap<RequestHeaderRefKind> map;
        map.insert("host", RequestHeaderRefKind::Host);
        map.insert("content-type", RequestHeaderRefKind::ContentType);
        map.insert("range", RequestHeaderRefKind::Range);
        map.insert("if-range", RequestHeaderRefKind::IfRange);
        map.insert("expect", RequestHeaderRefKind::Expect);
        return map;
    }();
    return refs;
}

} // namespace

HttpExchange::HttpExchange(mem::IoBufNodePool &node_pool, const HttpServerOptions &options,
                           net::SocketAddress remote_addr) :
    header_bufs_(node_pool), trailer_bufs_(node_pool), request_headers_(pool_), request_trailers_(pool_),
    remote_addr_(std::move(remote_addr)) {
    (void) options;
}

HttpExchange::~HttpExchange() = default;

std::string_view HttpExchange::header(std::string_view name) const noexcept { return request_headers_.get(name); }

void HttpExchange::set_io(HttpExchangeIo *io) noexcept { io_ = io; }

void HttpExchange::record_io_error(common::IoErr error) noexcept {
    if (response_stats_.terminal_error == common::IoErr::None) {
        response_stats_.terminal_error = error;
    }
}

void HttpExchange::cache_request_header_field(const HttpHeaders::HeaderField &field) noexcept {
    const auto *kind = request_header_ref_map().get(field.lowcase_view(), field.name_hash);
    if (kind == nullptr) {
        return;
    }
    switch (*kind) {
        case RequestHeaderRefKind::Host:
            if (request_header_refs_.host == nullptr) {
                request_header_refs_.host = &field;
            }
            break;
        case RequestHeaderRefKind::ContentType:
            if (request_header_refs_.content_type == nullptr) {
                request_header_refs_.content_type = &field;
            }
            break;
        case RequestHeaderRefKind::Range:
            if (request_header_refs_.range == nullptr) {
                request_header_refs_.range = &field;
            }
            break;
        case RequestHeaderRefKind::IfRange:
            if (request_header_refs_.if_range == nullptr) {
                request_header_refs_.if_range = &field;
            }
            break;
        case RequestHeaderRefKind::Expect:
            if (request_header_refs_.expect == nullptr) {
                request_header_refs_.expect = &field;
            }
            break;
    }
}

fiber::async::Task<common::IoResult<mem::IoBufChain>>
HttpExchange::read_body(std::size_t max_bytes, std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await io_->read_body(*this, max_bytes, timeout);
    if (!result) {
        record_io_error(result.error());
    }
    co_return std::move(result);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::discard_body(std::chrono::milliseconds timeout) noexcept {
    for (;;) {
        auto result = co_await read_body(4096, timeout);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->complete()) {
            break;
        }
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_header(const OutgoingHeaderBlockView &header,
                                                                     std::chrono::milliseconds timeout) {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await io_->send_header(*this, header, timeout);
    if (!result) {
        record_io_error(result.error());
        co_return result;
    }
    if (header.kind == OutgoingHeaderKind::Final) {
        response_stats_.status_code = header.status_code;
        response_stats_.header_sent = true;
        response_stats_.completed = header.end_stream;
    } else if (header.kind == OutgoingHeaderKind::Trailer && header.end_stream) {
        response_stats_.completed = true;
    }
    co_return result;
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_continue_header(std::chrono::milliseconds timeout) {
    co_return co_await send_header(
            {
                    .kind = OutgoingHeaderKind::Informational,
                    .status_code = 100,
                    .headers = nullptr,
                    .end_stream = false,
            },
            timeout);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_informational_header(int status_code,
                                                                                   const HttpHeaders *headers,
                                                                                   std::chrono::milliseconds timeout) {
    co_return co_await send_header(
            {
                    .kind = OutgoingHeaderKind::Informational,
                    .status_code = status_code,
                    .reason = {},
                    .headers = headers,
                    .body = HttpBodySpec::Auto(),
                    .connection_mode = ResponseConnectionMode::Auto,
                    .end_stream = false,
            },
            timeout);
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(mem::IoBufChain chunk,
                                                                      std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t intended = chunk.readable_bytes();
    const bool end = chunk.complete();
    auto result = co_await io_->write_body(*this, std::move(chunk), timeout);
    if (!result) {
        record_io_error(result.error());
        co_return result;
    }
    response_stats_.body_bytes_sent =
            *result > std::numeric_limits<std::size_t>::max() - response_stats_.body_bytes_sent
                    ? std::numeric_limits<std::size_t>::max()
                    : response_stats_.body_bytes_sent + *result;
    if (end && *result == intended) {
        response_stats_.completed = true;
    }
    co_return result;
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(const uint8_t *buf, size_t len, bool end,
                                                                      std::chrono::milliseconds timeout) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    auto result = co_await io_->write_body(*this, buf, len, end, timeout);
    if (!result) {
        record_io_error(result.error());
        co_return result;
    }
    response_stats_.body_bytes_sent =
            *result > std::numeric_limits<std::size_t>::max() - response_stats_.body_bytes_sent
                    ? std::numeric_limits<std::size_t>::max()
                    : response_stats_.body_bytes_sent + *result;
    if (end && *result == len) {
        response_stats_.completed = true;
    }
    co_return result;
}

common::IoResult<void> HttpExchange::abort(common::IoErr reason) noexcept {
    if (!io_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto result = io_->abort(*this, reason);
    record_io_error(result ? reason : result.error());
    return result;
}

} // namespace fiber::http
