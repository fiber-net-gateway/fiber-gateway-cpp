#ifndef FIBER_HTTP_HTTP1_CONNECTION_H
#define FIBER_HTTP_HTTP1_CONNECTION_H

#include <cstdint>
#include <memory>

#include <fiber/async/Task.h>
#include <fiber/common/IntrusiveList.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/Http1Parser.h>
#include <fiber/http/Http1ServerOptions.h>
#include <fiber/http/HttpExchange.h>

namespace fiber::http {

template<typename V>
class HeaderMap;

class Http1ConnectionRegistry;
class HttpTransport;

class Http1Connection : public common::NonCopyable, public common::NonMovable {
public:
    // `handler` must outlive the connection; pass `handler_owner` when the
    // caller wants the connection to keep it alive itself.
    Http1Connection(std::unique_ptr<HttpTransport> transport, const HttpHandler &handler, Http1ServerOptions options,
                    std::shared_ptr<const HttpHandler> handler_owner = nullptr);
    ~Http1Connection();

    fiber::async::Task<void> run();
    // Must be called on loop(). Closes the transport and wakes a pending read.
    void shutdown() noexcept;
    // Graceful stop, on loop(). An idle connection is closed right away; one
    // that is serving a request finishes it first, and its response carries
    // Connection: close (see Http1ExchangeIo::compute_close_conn). Idempotent.
    void request_drain() noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] HttpTransport &transport() noexcept { return *transport_; }
    [[nodiscard]] const Http1ServerOptions &options() const noexcept { return options_; }
    [[nodiscard]] mem::IoBufChain &inbound_bufs() noexcept { return inbound_bufs_; }
    [[nodiscard]] bool stopping() const noexcept;

private:
    using HeaderHandler = bool (*)(HttpExchange &exchange, const HttpHeaders::HeaderField &field);

    static const HeaderMap<HeaderHandler> &header_handler_map();
    static bool handle_content_length(HttpExchange &exchange, const HttpHeaders::HeaderField &header);
    static bool handle_transfer_encoding(HttpExchange &exchange, const HttpHeaders::HeaderField &header);
    static bool handle_connection(HttpExchange &exchange, const HttpHeaders::HeaderField &header);

    fiber::async::Task<common::IoResult<ParseCode>> parse_request(HttpExchange &exchange);
    std::size_t drain_inbound(mem::IoBuf &buffer) noexcept;
    void finish() noexcept;

    friend class Http1ConnectionRegistry;

    event::EventLoop &loop_;
    std::unique_ptr<HttpTransport> transport_;
    const HttpHandler *handler_;
    std::shared_ptr<const HttpHandler> handler_owner_;
    Http1ServerOptions options_;
    mem::IoBufChain inbound_bufs_;
    // Membership slot in the owning Http1ConnectionRegistry's list. Private
    // hook reached by offset, same pattern as
    // Http2ServerConnection::worker_hook_.
    common::IntrusiveListHook worker_hook_{};

    // Loop-affine state: every mutation happens on loop_.
    bool draining_ = false;
    bool idle_ = false;
    bool finished_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_H
