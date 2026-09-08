#ifndef FIBER_HTTP_ENDPOINT_HTTP2_ENDPOINT_H
#define FIBER_HTTP_ENDPOINT_HTTP2_ENDPOINT_H

#include <cstddef>
#include <memory>

#include "../Http1ServerOptions.h"
#include "../Http2Connection.h"
#include "../Http2ServerOptions.h"
#include "../ServerRequestFactory.h"
#include "Http1ConnectionRegistry.h"
#include "Http2ConnectionRegistry.h"
#include "TcpEndpointBase.h"

namespace fiber::http {

// One HTTP/2 endpoint's presence on a single worker loop. It keeps both
// registries because a single endpoint serves HTTP/2 and, when it is allowed to
// negotiate down, HTTP/1 sessions side by side.
class Http2EndpointWorker final : public TcpEndpointWorkerBase {
public:
    using TcpEndpointWorkerBase::TcpEndpointWorkerBase;

    [[nodiscard]] Http2ConnectionRegistry &http2_connections() noexcept { return http2_; }
    [[nodiscard]] Http1ConnectionRegistry &http1_connections() noexcept { return http1_; }

    void drain() noexcept override {
        TcpEndpointWorkerBase::drain();
        http2_.drain_all();
        http1_.drain_all();
    }

private:
    Http2ConnectionRegistry http2_{};
    Http1ConnectionRegistry http1_{};
};

// HTTP/2 endpoint, optionally negotiating down to HTTP/1.1 (`allow_http1`).
//
// One class covers both because the two differ only in the ALPN set offered and
// which sessions the worker then has to run; a separate "h2 or h1" endpoint
// type would duplicate the accept path, the request factory and the drain
// walk for one boolean.
//
// `Upgrade: h2c` is deliberately not supported (design decision D6). A
// plaintext port therefore cannot negotiate at all, and `allow_http1` picks
// which protocol it speaks outright -- see the field comment.
class Http2Endpoint final : public TcpEndpointBase {
public:
    struct Options {
        net::SocketAddress address{};
        net::ListenOptions listen{};
        net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
        // Leaving this unset (no configure callback) serves plaintext.
        HttpServerTlsOptions tls{};
        Http2ServerOptions http2{};
        // Only consulted for sessions that negotiate down to HTTP/1.
        Http1ServerOptions http1{};
        // With TLS:   true  -> offer ALPN {h2, http/1.1} and serve whichever the
        //                      peer selects (also HTTP/1 for a peer that sends
        //                      no ALPN at all);
        //             false -> offer {h2} only, and close a session that ends up
        //                      on anything else.
        // Plaintext:  there is nothing to negotiate and Upgrade: h2c is not
        //             supported, so true -> HTTP/1, false -> HTTP/2 with prior
        //             knowledge (the peer must open with the connection
        //             preface).
        bool allow_http1 = true;
        // Empty falls back to the Server's default handler.
        HttpHandler handler{};
    };

    explicit Http2Endpoint(Options options) noexcept;

    [[nodiscard]] bool allows_http1() const noexcept { return options_.allow_http1; }

protected:
    [[nodiscard]] common::IoResult<void> on_start(Server &server) noexcept override;
    [[nodiscard]] TcpEndpointWorkerBase *make_worker(event::EventLoop &loop, std::size_t index) noexcept override;
    [[nodiscard]] net::TlsServerParam make_tls_param() const noexcept override;
    [[nodiscard]] async::Task<void> serve_connection(TcpEndpointWorkerBase &worker,
                                                     std::unique_ptr<HttpTransport> transport) noexcept override;

private:
    enum class Protocol : std::uint8_t { Http1, Http2, Unsupported };

    [[nodiscard]] Protocol select_protocol(std::string_view alpn) const noexcept;
    [[nodiscard]] Http2Connection::Options make_connection_options() const noexcept;

    async::Task<void> serve_http1(Http2EndpointWorker &worker, std::unique_ptr<HttpTransport> transport) noexcept;
    async::Task<void> serve_http2(Http2EndpointWorker &worker, std::unique_ptr<HttpTransport> transport) noexcept;

    Options options_;
    // Both resolved in on_start(); shared so a session outlives this facade if
    // the endpoint is ever torn down first.
    std::shared_ptr<const HttpHandler> handler_{};
    std::unique_ptr<ServerRequestFactory> request_factory_{};
};

} // namespace fiber::http

#endif // FIBER_HTTP_ENDPOINT_HTTP2_ENDPOINT_H
