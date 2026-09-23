#ifndef FIBER_HTTP_HTTP_SERVER_TLS_OPTIONS_H
#define FIBER_HTTP_HTTP_SERVER_TLS_OPTIONS_H

#include <cstdint>
#include <string>
#include <vector>

#include "../net/TlsParams.h"

namespace fiber::http {

// One ticket-protection key for stateless session tickets (09 §6), expressed
// as deployable config: the raw 16-byte AES-128 key in hex. The TCP endpoints
// assemble a net::TlsTicketService from these at start; a restart that feeds
// the same material keeps every issued ticket openable (stateless). Keys are
// minted in [created_ms, created_ms + 24 h) and open for 7 more days.
struct HttpServerTlsTicketKey {
    std::uint32_t id = 0; // unique within the set; rides in every ticket
    std::string key_hex; // exactly 32 hex chars (16 bytes)
    std::int64_t created_ms = 0; // Unix epoch milliseconds; the window origin
};

// TLS policy shared by the HTTP endpoints. ALPN is not
// configurable here: each server advertises the fixed protocol set it
// implements (see TlsAlpn.h's make_*_server_tls_param helpers).
struct HttpServerTlsOptions {
    net::ConfigureTlsCallback configure_callback = nullptr;
    void *configure_ctx = nullptr;
    const net::TrustStore *trust_store = nullptr;
    net::TlsClientCertificateMode client_certificate_mode = net::TlsClientCertificateMode::None;
    int min_version = 0x0303; // TLS 1.2
    int max_version = 0x0304; // TLS 1.3
    bool enable_early_data = false;
    // Stateless session tickets (09 §6). Empty (the default) = tickets off:
    // the server sends no NewSessionTicket and resumes nothing. Non-empty is
    // validated at server start (hex width, unique ids, at most 8 keys) and
    // a failure refuses to start. TCP endpoints only: the HTTP/3 endpoint
    // ignores the field (QUIC's TLS is out of the 09 re-core's scope).
    std::vector<HttpServerTlsTicketKey> ticket_keys{};

    [[nodiscard]] bool enabled() const noexcept { return configure_callback != nullptr; }
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_SERVER_TLS_OPTIONS_H
