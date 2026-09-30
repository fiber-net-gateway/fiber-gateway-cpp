#ifndef FIBER_HTTP_HTTP_EXCHANGE_H
#define FIBER_HTTP_HTTP_EXCHANGE_H

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "../async/Task.h"
#include "../async/WaitAwaiter.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../common/mem/BufPool.h"
#include "../common/mem/IoBufChain.h"
#include "../event/EventLoop.h"
#include "../net/SocketAddress.h"
#include "HttpCommon.h"
#include "HttpExchangeIo.h"
#include "HttpHeaders.h"

namespace fiber::http {

class Http1Connection;
class Http1ExchangeIo;
class HttpTransport;
class RequestLineParser;
class HeaderLineParser;
class ServerHttp2Request;
class ServerHttp3Request;

struct HttpResponseStats {
    int status_code = 0;
    std::size_t body_bytes_sent = 0;
    common::IoErr terminal_error = common::IoErr::None;
    bool header_sent = false;
    bool completed = false;
};

class HttpExchange : public common::NonCopyable, public common::NonMovable {
public:
    class ResponseChannelClosedAwaiter;

    struct RequestHeaderRefs {
        const HttpHeaders::HeaderField *host = nullptr;
        const HttpHeaders::HeaderField *content_type = nullptr;
        const HttpHeaders::HeaderField *range = nullptr;
        const HttpHeaders::HeaderField *if_range = nullptr;
        const HttpHeaders::HeaderField *expect = nullptr;
        const HttpHeaders::HeaderField *accept_encoding = nullptr;
    };

    explicit HttpExchange(net::SocketAddress remote_addr);
    ~HttpExchange();

    [[nodiscard]] HttpMethod method() const noexcept { return method_; }
    [[nodiscard]] HttpVersion version() const noexcept { return version_; }
    [[nodiscard]] const HttpUri &uri() const noexcept { return uri_; }
    [[nodiscard]] std::string_view version_view() const noexcept { return version_view_; }
    [[nodiscard]] std::string_view method_view() const noexcept { return method_view_; }
    [[nodiscard]] std::string_view scheme() const noexcept { return scheme_view_; }
    [[nodiscard]] std::string_view protocol() const noexcept { return protocol_view_; }
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
    [[nodiscard]] const RequestHeaderRefs &request_header_refs() const noexcept { return request_header_refs_; }
    [[nodiscard]] const HttpHeaders::HeaderField *host_header() const noexcept { return request_header_refs_.host; }
    [[nodiscard]] const HttpHeaders::HeaderField *content_type_header() const noexcept {
        return request_header_refs_.content_type;
    }
    [[nodiscard]] const HttpHeaders::HeaderField *range_header() const noexcept { return request_header_refs_.range; }
    [[nodiscard]] const HttpHeaders::HeaderField *if_range_header() const noexcept {
        return request_header_refs_.if_range;
    }
    [[nodiscard]] const HttpHeaders::HeaderField *expect_header() const noexcept { return request_header_refs_.expect; }
    [[nodiscard]] const HttpHeaders::HeaderField *accept_encoding_header() const noexcept {
        return request_header_refs_.accept_encoding;
    }
    [[nodiscard]] const HttpHeaders &request_headers() const noexcept { return request_headers_; };
    [[nodiscard]] const HttpHeaders &request_trailers() const noexcept { return request_trailers_; };
    [[nodiscard]] HttpBodySpec request_body_spec() const noexcept { return request_body_spec_; }
    [[nodiscard]] bool request_trailers_complete() const noexcept { return request_trailers_complete_; }
    mem::BufPool &pool() noexcept { return pool_; }

    // --- Mutable request-phase access (HttpHandler context only) ---------------
    //
    // 1. Framing state is frozen at parse time and never re-derived from the
    //    table (request_body_spec(), keep-alive/close decisions, H2/H3 stream
    //    framing). NEVER rewrite the protocol-control names: content-length,
    //    transfer-encoding, connection, te, trailer, expect, upgrade,
    //    sec-websocket-*. Doing so desyncs the table from wire truth already
    //    consumed. accept-encoding / cache-control are safe: gzip negotiation
    //    reads them lazily at first response write.
    // 2. Use only the copying overloads (add / add_prehashed / set): they
    //    pool-copy name and value. The add_view / set_view family retains
    //    external pointers and dangles here.
    // 3. refs cache the FIRST matching field per name (first-wins, wire
    //    order). After any structural change you must fix the affected ref
    //    yourself: set() returns the new field — assign it directly; after
    //    remove(), null the ref or re-lookup via get_all(lowcase, hash).begin().
    //    Stale refs silently read the orphaned old value. Current post-parse
    //    readers: host (access log), expect (100-continue limiter),
    //    accept_encoding (gzip decision).
    // 4. uri() returns four coupled views with no storage of their own:
    //    reseat only to exchange-pool storage (pool().alloc + memcpy), never
    //    coroutine-frame or temporary strings; keep path (decoded) /
    //    unparsed_uri (raw) / query coherent — the proxy target builder
    //    prefers unparsed_uri wholesale and takes the query suffix from it
    //    (HttpProxyCore.h request_target_view, ProxyHandler raw_query_suffix);
    //    a new path must stay origin-form; route matching is not re-run.
    // 5. Timing: mutate before the first $header script access
    //    (ScriptExchangeCtx materializes the header object once). $path /
    //    $query / $req.uri read live per access.
    // 6. request_trailers() intentionally has no non-const overload: the
    //    parser appends to it while the request body is being read.
    [[nodiscard]] HttpHeaders &request_headers() noexcept { return request_headers_; }
    [[nodiscard]] RequestHeaderRefs &request_header_refs() noexcept { return request_header_refs_; }
    [[nodiscard]] HttpUri &uri() noexcept { return uri_; }
    [[nodiscard]] const net::SocketAddress &remote_addr() const noexcept { return remote_addr_; }
    [[nodiscard]] const HttpResponseStats &response_stats() const noexcept { return response_stats_; }
    // This is response-direction state: request EOF/END_STREAM alone does not
    // close the channel while the peer can still receive a response.
    [[nodiscard]] bool response_channel_closed() const noexcept;
    [[nodiscard]] ResponseChannelClosedAwaiter wait_response_channel_closed() noexcept;

    fiber::async::Task<common::IoResult<mem::IoBufChain>>
    read_body(std::size_t max_bytes, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<void>>
    discard_body(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;

    fiber::async::Task<common::IoResult<void>>
    send_header(const OutgoingHeaderBlockView &header,
                std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    fiber::async::Task<common::IoResult<void>>
    send_continue_header(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    fiber::async::Task<common::IoResult<void>>
    send_informational_header(int status_code, const HttpHeaders *headers = nullptr,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    // write_all transfers the entire body chunk, including its completion
    // marker, before succeeding.
    fiber::async::Task<common::IoResult<size_t>>
    write_all(mem::IoBufChain chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<size_t>>
    write_all(const uint8_t *buf, size_t len, bool end,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    // write returns after the send stack accepts at least one body byte, or
    // after accepting a zero-length completion marker. The chain overload
    // consumes only that prefix and leaves completion set until the final byte
    // and protocol terminator are accepted. After a successful partial write,
    // retry the exact remaining suffix with the same completion/end value.
    // TimedOut and other terminal transport errors abort this response; no
    // subsequent header or body write is allowed.
    fiber::async::Task<common::IoResult<size_t>>
    write(mem::IoBufChain &chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    fiber::async::Task<common::IoResult<size_t>>
    write(const uint8_t *buf, size_t len, bool end,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) noexcept;


private:
    void set_io(HttpExchangeIo *io) noexcept;
    void cache_request_header_field(const HttpHeaders::HeaderField &field) noexcept;
    void record_io_error(common::IoErr error) noexcept;
    void record_response_write_error(common::IoErr error) noexcept;

    friend class RequestLineParser;
    friend class HeaderLineParser;
    friend class Http1Connection;
    friend class Http1ExchangeIo;
    friend class ServerHttp2Request;
    friend class ServerHttp3Request;

    fiber::mem::BufPool pool_;
    fiber::mem::IoBufChain header_bufs_;
    fiber::mem::IoBufChain trailer_bufs_;
    HttpMethod method_{};
    HttpVersion version_{};
    HttpUri uri_;
    std::string_view method_view_;
    std::string_view version_view_;
    std::string_view scheme_view_;
    std::string_view protocol_view_;
    HttpHeaders request_headers_;
    HttpHeaders request_trailers_;
    bool request_trailers_complete_ = false;
    RequestHeaderRefs request_header_refs_;
    HttpBodySpec request_body_spec_{HttpBodySpec::None()};
    net::SocketAddress remote_addr_{};
    HttpResponseStats response_stats_{};
    common::IoErr response_write_error_ = common::IoErr::None;
    bool request_close_ = false;
    bool request_keep_alive_ = false;
    HttpExchangeIo *io_ = nullptr;
    ResponseChannelClosedAwaiter *response_channel_waiter_ = nullptr;
};

// Parks a coroutine on the io's one-shot channel-closed notification. The io
// side stays a plain query plus set/clear callback; this adapter adds the
// suspension machinery via WaitAwaiter: retractable local-defer resume, so a
// coroutine hard-destroyed by when_any with a queued resume still tears down
// safely. The io may fire the notification synchronously from inside
// registration (H2 when the channel is already closed); the waiter claims the
// exchange slot and parks before registering so that completion finds a
// consistent waiter.
class HttpExchange::ResponseChannelClosedAwaiter : public fiber::async::WaitAwaiter {
public:
    explicit ResponseChannelClosedAwaiter(HttpExchange &exchange) noexcept;
    ~ResponseChannelClosedAwaiter() noexcept;

    bool await_ready() noexcept;
    bool await_suspend(std::coroutine_handle<> continuation) noexcept;
    common::IoResult<void> await_resume() noexcept;

private:
    static void on_response_channel_closed(void *ctx) noexcept;
    static void detach_from_exchange(fiber::async::WaitAwaiter &base) noexcept;

    HttpExchange *exchange_ = nullptr;
    HttpExchangeIo *registered_io_ = nullptr;
};

using HttpHandler = std::function<fiber::async::Task<void>(HttpExchange &)>;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_EXCHANGE_H
