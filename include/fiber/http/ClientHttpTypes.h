#ifndef FIBER_HTTP_CLIENT_HTTP_TYPES_H
#define FIBER_HTTP_CLIENT_HTTP_TYPES_H

#include <string_view>

#include "../common/mem/BufPool.h"
#include "HttpBodySpec.h"
#include "HttpCommon.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"

namespace fiber::http {

// One request head for every client protocol. The pseudo-header fields carry HTTP/2 and HTTP/3
// semantics directly; HTTP/1 derives its request line and Host from the same values, so a caller
// that fills this in once can send it over any of the three.
//
// Non-owning: every view and `headers` is borrowed until the send_header task completes.
struct ClientRequestHead {
    HttpMethod method = HttpMethod::Unknown;
    // HTTP/2 and HTTP/3 :path. For HTTP/1 this is the request-target verbatim, so it also carries
    // the absolute, authority, and asterisk forms.
    std::string_view path{};
    // HTTP/2 and HTTP/3 :scheme. HTTP/1 ignores it: the scheme is a property of the transport.
    std::string_view scheme{};
    // HTTP/2 and HTTP/3 :authority. For HTTP/1 a non-empty value is encoded as the Host header and
    // *overrides* any host field in `headers`; leave it empty to send the one in `headers`.
    std::string_view authority{};
    // Extended CONNECT (:protocol). HTTP/1 rejects a non-empty value with NotSupported.
    std::string_view protocol{};
    const HttpHeaders *headers = nullptr;
    // HTTP/1 framing: Content-Length, chunked, or a raw stream after the final header. HTTP/2 and
    // HTTP/3 frame from `end_stream` plus the caller's own content-length header and only reject
    // Chunked, which has no meaning there.
    HttpBodySpec body = HttpBodySpec::None();
};

// One response head for every client protocol, filled by whichever implementation read it.
//
// `headers` is owned; the views inside it point into buffers the exchange retains, so a head stays
// readable until the next read_header on the same exchange or the exchange's destruction.
struct ClientResponseHead {
    explicit ClientResponseHead(mem::BufPool &pool, HttpVersion protocol_version = HttpVersion::HTTP_1_1) noexcept :
        version(protocol_version), headers(pool) {}

    HttpVersion version = HttpVersion::HTTP_1_1;
    OutgoingHeaderKind kind = OutgoingHeaderKind::Final;
    int status_code = 0;
    // HTTP/2 and HTTP/3 report this exactly. HTTP/1 can only set it when the header block itself
    // proves no body follows (a HEAD response, 204, 304, or a zero Content-Length), so the
    // portable end-of-body test across all three is read_body()'s complete() instead.
    bool end_stream = false;
    // HTTP/1 reason phrase. Empty for HTTP/2 and HTTP/3, which have none.
    std::string_view reason{};
    HttpHeaders headers;

    [[nodiscard]] bool is_informational() const noexcept { return kind == OutgoingHeaderKind::Informational; }
    [[nodiscard]] bool is_trailer() const noexcept { return kind == OutgoingHeaderKind::Trailer; }
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP_TYPES_H
