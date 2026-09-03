#ifndef FIBER_HTTP_HTTP1_CONNECTION_H
#define FIBER_HTTP_HTTP1_CONNECTION_H

#include <atomic>
#include <cstdint>
#include <memory>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>
#include <fiber/http/Http1Parser.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/http/HttpServerOptions.h>

namespace fiber::http {

template<typename V>
class HeaderMap;

class Http1Server;
class HttpTransport;

class Http1Connection : public common::NonCopyable, public common::NonMovable {
public:
    // shutdown_flag is an optional externally-owned cancellation flag. When
    // supplied, its lifetime must cover this connection.
    Http1Connection(Http1Server *server, std::unique_ptr<HttpTransport> transport, HttpHandler handler,
                    HttpServerOptions options, const std::atomic<bool> *shutdown_flag = nullptr);
    ~Http1Connection();

    fiber::async::Task<void> run();
    // Must be called on loop(). Closes the transport and wakes a pending read.
    void shutdown() noexcept;

    [[nodiscard]] event::EventLoop &loop() const noexcept { return loop_; }
    [[nodiscard]] HttpTransport &transport() noexcept { return *transport_; }
    [[nodiscard]] const HttpServerOptions &options() const noexcept { return options_; }
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

    Http1Server *server_ = nullptr;
    const std::atomic<bool> *shutdown_flag_ = nullptr;
    event::EventLoop &loop_;
    std::unique_ptr<HttpTransport> transport_;
    HttpHandler handler_;
    HttpServerOptions options_;
    mem::IoBufChain inbound_bufs_;

    std::atomic<bool> finished_{false};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_H
