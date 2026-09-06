#ifndef FIBER_HTTP_CLIENT_HTTP_EXCHANGE_H
#define FIBER_HTTP_CLIENT_HTTP_EXCHANGE_H

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "ClientHttp1Exchange.h"
#include "ClientHttp2Exchange.h"
#include "ClientHttp3Exchange.h"
#include "ClientHttpTypes.h"
#include "HttpClientDialer.h"

namespace fiber::http {

// One request over whichever HTTP version the connection speaks. The three protocol exchanges
// already share ClientRequestHead and ClientResponseHead and expose the same operations, so this
// is a tagged pointer that forwards to one of them: every method below is a switch and a return,
// with no wrapper coroutine frame between the caller and the protocol implementation.
//
// Non-owning. The concrete exchange it points at, and everything that exchange borrows (its
// connection, buffer pool, and stream lease), must outlive this handle and every Task it returns.
// Protocol-specific operations that have no cross-version meaning — HTTP/1's raw-stream upgrade,
// HTTP/2 stream ids, HTTP/3 request outcomes — are reached through as_http1() and friends.
class ClientHttpExchange {
public:
    ClientHttpExchange() noexcept = default;
    explicit ClientHttpExchange(ClientHttp1Exchange &exchange) noexcept :
        protocol_(HttpProtocol::Http1), http1_(&exchange) {}
    explicit ClientHttpExchange(ClientHttp2Exchange &exchange) noexcept :
        protocol_(HttpProtocol::Http2), http2_(&exchange) {}
    explicit ClientHttpExchange(ClientHttp3Exchange &exchange) noexcept :
        protocol_(HttpProtocol::Http3), http3_(&exchange) {}

    // HTTP/1 rejects a Chunked body spec only when the peer cannot accept it; HTTP/2 and HTTP/3
    // reject it outright, and both ignore the rest of ClientRequestHead::body. See
    // ClientRequestHead for how each field maps onto the wire.
    fiber::async::Task<common::IoResult<void>>
    send_header(const ClientRequestHead &head, bool end_stream,
                std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->send_header(head, end_stream, timeout);
            case HttpProtocol::Http2:
                return http2_->send_header(head, end_stream, timeout);
            case HttpProtocol::Http3:
                return http3_->send_header(head, end_stream, timeout);
        }
        return invalid_void();
    }

    // write_all accepts the complete payload before returning. write returns after the first
    // batch the send stack accepts and consumes an IoBufChain in place; retry the exact remaining
    // suffix with the same end_stream value.
    fiber::async::Task<common::IoResult<std::size_t>>
    write_all(mem::IoBufChain chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->write_all(std::move(chunk), timeout);
            case HttpProtocol::Http2:
                return http2_->write_all(std::move(chunk), timeout);
            case HttpProtocol::Http3:
                return http3_->write_all(std::move(chunk), timeout);
        }
        return invalid_size();
    }

    fiber::async::Task<common::IoResult<std::size_t>>
    write_all(const std::uint8_t *buf, std::size_t len, bool end_stream,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->write_all(buf, len, end_stream, timeout);
            case HttpProtocol::Http2:
                return http2_->write_all(buf, len, end_stream, timeout);
            case HttpProtocol::Http3:
                return http3_->write_all(buf, len, end_stream, timeout);
        }
        return invalid_size();
    }

    fiber::async::Task<common::IoResult<std::size_t>>
    write(mem::IoBufChain &chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->write(chunk, timeout);
            case HttpProtocol::Http2:
                return http2_->write(chunk, timeout);
            case HttpProtocol::Http3:
                return http3_->write(chunk, timeout);
        }
        return invalid_size();
    }

    fiber::async::Task<common::IoResult<std::size_t>>
    write(const std::uint8_t *buf, std::size_t len, bool end_stream,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->write(buf, len, end_stream, timeout);
            case HttpProtocol::Http2:
                return http2_->write(buf, len, end_stream, timeout);
            case HttpProtocol::Http3:
                return http3_->write(buf, len, end_stream, timeout);
        }
        return invalid_size();
    }

    fiber::async::Task<common::IoResult<void>>
    send_trailer(const HttpHeaders &trailers,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->send_trailer(trailers, timeout);
            case HttpProtocol::Http2:
                return http2_->send_trailer(trailers, timeout);
            case HttpProtocol::Http3:
                return http3_->send_trailer(trailers, timeout);
        }
        return invalid_void();
    }

    // Delivers the response's header blocks in order — informational, then final, then a trailer
    // block if one arrives — and reports the end of that sequence with a successful null head.
    // The returned head stays readable until the next read_header on the same exchange.
    fiber::async::Task<common::IoResult<const ClientResponseHead *>>
    read_header(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->read_header(timeout);
            case HttpProtocol::Http2:
                return http2_->read_header(timeout);
            case HttpProtocol::Http3:
                return http3_->read_header(timeout);
        }
        return invalid_head();
    }

    fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes = 64 * 1024,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->read_body(max_bytes, timeout);
            case HttpProtocol::Http2:
                return http2_->read_body(max_bytes, timeout);
            case HttpProtocol::Http3:
                return http3_->read_body(max_bytes, timeout);
        }
        return invalid_body();
    }

    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept {
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_->abort(reason);
            case HttpProtocol::Http2:
                return http2_->abort(reason);
            case HttpProtocol::Http3:
                return http3_->abort(reason);
        }
        return std::unexpected(common::IoErr::Invalid);
    }

    [[nodiscard]] HttpProtocol protocol() const noexcept { return protocol_; }
    [[nodiscard]] HttpVersion version() const noexcept { return http_protocol_version(protocol_); }
    [[nodiscard]] bool valid() const noexcept {
        // Reads the arm the tag selects; a default-constructed handle has a null Http1 arm.
        switch (protocol_) {
            case HttpProtocol::Http1:
                return http1_ != nullptr;
            case HttpProtocol::Http2:
                return http2_ != nullptr;
            case HttpProtocol::Http3:
                return http3_ != nullptr;
        }
        return false;
    }

    [[nodiscard]] ClientHttp1Exchange *as_http1() noexcept {
        return protocol_ == HttpProtocol::Http1 ? http1_ : nullptr;
    }
    [[nodiscard]] ClientHttp2Exchange *as_http2() noexcept {
        return protocol_ == HttpProtocol::Http2 ? http2_ : nullptr;
    }
    [[nodiscard]] ClientHttp3Exchange *as_http3() noexcept {
        return protocol_ == HttpProtocol::Http3 ? http3_ : nullptr;
    }

private:
    // Unreachable through the public constructors, which always set a protocol alongside a
    // backend. They exist so a default-constructed handle fails loudly instead of dereferencing
    // null, and so every switch above is total.
    static fiber::async::Task<common::IoResult<void>> invalid_void() noexcept;
    static fiber::async::Task<common::IoResult<std::size_t>> invalid_size() noexcept;
    static fiber::async::Task<common::IoResult<const ClientResponseHead *>> invalid_head() noexcept;
    static fiber::async::Task<common::IoResult<mem::IoBufChain>> invalid_body() noexcept;

    HttpProtocol protocol_ = HttpProtocol::Http1;
    union {
        ClientHttp1Exchange *http1_ = nullptr;
        ClientHttp2Exchange *http2_;
        ClientHttp3Exchange *http3_;
    };
};

} // namespace fiber::http

#endif // FIBER_HTTP_CLIENT_HTTP_EXCHANGE_H
