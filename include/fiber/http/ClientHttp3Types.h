#ifndef FIBER_HTTP_CLIENT_HTTP3_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP3_TYPES_H

#include <cstdint>
#include <string_view>

#include "HttpCommon.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"

namespace fiber::http {

enum class Http3ExtendedConnectSupport : std::uint8_t {
    Unknown,
    Disabled,
    Enabled,
};

enum class Http3RequestOutcome : std::uint8_t {
    NotSent,
    Rejected,
    PossiblyProcessed,
    Complete,
};

struct Http3RequestHead {
    HttpMethod method = HttpMethod::Unknown;
    std::string_view scheme{};
    std::string_view authority{};
    std::string_view path{};
    std::string_view protocol{};
    const HttpHeaders *headers = nullptr;
};

struct Http3ResponseHead {
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    bool end_stream = false;
    HttpHeaders headers;

    explicit Http3ResponseHead(mem::BufPool &pool) noexcept : headers(pool) {}
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP3_TYPES_H
