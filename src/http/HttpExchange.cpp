#include "HttpExchange.h"

#include <cstring>
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

HttpExchange::HttpExchange(mem::IoBufNodePool &node_pool, const HttpServerOptions &options) :
    header_bufs_(node_pool), trailer_bufs_(node_pool), request_headers_(pool_), request_trailers_(pool_) {
    (void) options;
}

HttpExchange::~HttpExchange() = default;

std::string_view HttpExchange::header(std::string_view name) const noexcept { return request_headers_.get(name); }

void HttpExchange::set_io(HttpExchangeIo *io) noexcept { io_ = io; }

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

fiber::async::Task<common::IoResult<BodyChunk>> HttpExchange::read_body(std::size_t max_bytes) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->read_body(*this, max_bytes);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::discard_body() noexcept {
    for (;;) {
        auto result = co_await read_body(4096);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        if (result->last) {
            break;
        }
    }
    co_return common::IoResult<void>{};
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_header(const OutgoingHeaderBlockView &header) {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->send_header(*this, header);
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_continue_header() {
    co_return co_await send_header({
            .kind = OutgoingHeaderKind::Informational,
            .status_code = 100,
            .headers = nullptr,
            .end_stream = false,
    });
}

fiber::async::Task<common::IoResult<void>> HttpExchange::send_informational_header(int status_code,
                                                                                   const HttpHeaders *headers) {
    co_return co_await send_header({
            .kind = OutgoingHeaderKind::Informational,
            .status_code = status_code,
            .reason = {},
            .headers = headers,
            .body = HttpBodySpec::Auto(),
            .connection_mode = ResponseConnectionMode::Auto,
            .end_stream = false,
    });
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(BodyChunk chunk) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->write_body(*this, std::move(chunk));
}

fiber::async::Task<common::IoResult<size_t>> HttpExchange::write_body(const uint8_t *buf, size_t len,
                                                                      bool end) noexcept {
    if (!io_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return co_await io_->write_body(*this, buf, len, end);
}

} // namespace fiber::http
