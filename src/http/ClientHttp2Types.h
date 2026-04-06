#ifndef FIBER_HTTP_CLIENT_HTTP2_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP2_TYPES_H

#include <string_view>

#include "HttpCommon.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"

namespace fiber::http {

struct Http2RequestHead {
    HttpMethod method = HttpMethod::Unknown;
    std::string_view scheme{};
    std::string_view authority{};
    std::string_view path{};
    const HttpHeaders *headers = nullptr;
};

struct Http2ResponseHead {
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    bool end_stream = false;
    HttpHeaders headers;

    explicit Http2ResponseHead(mem::BufPool &pool) : headers(pool) {}
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP2_TYPES_H
