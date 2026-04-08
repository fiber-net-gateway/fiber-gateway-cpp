#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <lsquic.h>
#include <lsxpack_header.h>
#include <openssl/ssl.h>
#include <lsquic_logger.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "net/SocketAddress.h"
#include "net/UdpPacket.h"
#include "net/UdpSocket.h"

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::event::IoEvent;

constexpr size_t kMaxDatagram = 65535;
constexpr std::string_view kBody = "hello h3 via lsquic\n";
constexpr unsigned char kH3AlpnWire[] = {2, 'h', '3'};
constexpr auto kStopPollInterval = std::chrono::milliseconds(200);

struct ServerCtx;

struct ConnCtx {
};

struct StreamCtx {
    std::string request;
    size_t body_offset = 0;
    bool ready_to_respond = false;
    bool headers_sent = false;
};

struct ServerCtx {
    fiber::event::EventLoop *loop = nullptr;
    fiber::net::UdpSocket *udp = nullptr;
    uint16_t port = 8443;
    lsquic_engine_t *engine = nullptr;
    lsquic_engine_settings settings{};
    SSL_CTX *ssl_ctx = nullptr;
    std::atomic<bool> stop{false};
    bool tx_blocked = false;
};

ServerCtx *g_server = nullptr;

struct HeaderBuf {
    unsigned off = 0;
    std::array<char, 1024> buf{};
};

bool is_v4_mapped_ipv6(const std::array<std::uint8_t, 16> &bytes) {
    for (std::size_t i = 0; i < 10; ++i) {
        if (bytes[i] != 0) {
            return false;
        }
    }
    return bytes[10] == 0xFF && bytes[11] == 0xFF;
}

fiber::net::SocketAddress normalize_socket_address(const fiber::net::SocketAddress &addr) {
    if (!addr.ip().is_v6()) {
        return addr;
    }
    const auto &bytes = addr.ip().v6_bytes();
    if (!is_v4_mapped_ipv6(bytes)) {
        return addr;
    }
    std::array<std::uint8_t, 4> v4{};
    std::memcpy(v4.data(), bytes.data() + 12, v4.size());
    return {fiber::net::IpAddress::v4(v4), addr.port()};
}

bool parse_port(const char *text, uint16_t &out) {
    if (!text) {
        return false;
    }
    char *end = nullptr;
    unsigned long v = std::strtoul(text, &end, 10);
    if (!end || *end != '\0' || v > 65535UL) {
        return false;
    }
    out = static_cast<uint16_t>(v);
    return true;
}

void configure_lsquic_logging() {
    const char *spec = std::getenv("H3_DEMO_LSQUIC_LOG");
    if (!spec || *spec == '\0') {
        return;
    }
    lsquic_log_to_fstream(stderr, LLTS_HHMMSSUS);
    if (0 != lsquic_logger_lopt(spec)) {
        std::cerr << "invalid lsquic log spec: " << spec << '\n';
    }
}

int select_alpn_cb(SSL *ssl,
                   const unsigned char **out,
                   unsigned char *outlen,
                   const unsigned char *in,
                   unsigned int inlen,
                   void *arg) {
    (void) ssl;
    (void) arg;
    int rc = SSL_select_next_proto(const_cast<unsigned char **>(out),
                                   outlen,
                                   in,
                                   inlen,
                                   kH3AlpnWire,
                                   sizeof(kH3AlpnWire));
    return rc == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
}

bool bind_udp_socket(fiber::net::UdpSocket &socket, uint16_t port) {
    fiber::net::UdpBindOptions options{};
    options.reuse_addr = true;
    options.reuse_port = true;
    options.v6_only = false;
    options.recv_packet_info = true;
    options.recv_ecn = true;
    auto bind_result = socket.bind(fiber::net::SocketAddress::any_v6(port), options);
    return static_cast<bool>(bind_result);
}

int header_set_ptr(lsxpack_header *hdr,
                   HeaderBuf &header_buf,
                   std::string_view name,
                   std::string_view value) {
    if (header_buf.off + name.size() + value.size() > header_buf.buf.size()) {
        return -1;
    }
    char *dst = header_buf.buf.data() + header_buf.off;
    std::memcpy(dst, name.data(), name.size());
    std::memcpy(dst + name.size(), value.data(), value.size());
    lsxpack_header_set_offset2(hdr,
                               dst,
                               0,
                               name.size(),
                               name.size(),
                               value.size());
    header_buf.off += static_cast<unsigned>(name.size() + value.size());
    return 0;
}

fiber::net::UdpEcn to_udp_ecn(int ecn) {
    switch (ecn & 0x03) {
        case 0:
            return fiber::net::UdpEcn::NonEct;
        case 1:
            return fiber::net::UdpEcn::Ect1;
        case 2:
            return fiber::net::UdpEcn::Ect0;
        case 3:
            return fiber::net::UdpEcn::Ce;
        default:
            return fiber::net::UdpEcn::Unspecified;
    }
}

int to_lsquic_ecn(fiber::net::UdpEcn ecn) {
    if (ecn == fiber::net::UdpEcn::Unspecified) {
        return 0;
    }
    return static_cast<int>(ecn) & 0x03;
}

bool to_socket_address(const sockaddr *sa, fiber::net::SocketAddress &out) {
    if (!sa) {
        return false;
    }
    socklen_t len = 0;
    switch (sa->sa_family) {
        case AF_INET:
            len = sizeof(sockaddr_in);
            break;
        case AF_INET6:
            len = sizeof(sockaddr_in6);
            break;
        default:
            return false;
    }
    fiber::net::SocketAddress parsed;
    if (!fiber::net::SocketAddress::from_sockaddr(sa, len, parsed)) {
        return false;
    }
    out = normalize_socket_address(parsed);
    return true;
}

bool fill_send_spec(const lsquic_out_spec &src, fiber::net::UdpPacketSendSpec &dst) {
    if (!to_socket_address(src.dest_sa, dst.peer)) {
        return false;
    }
    dst.iov = src.iov;
    if (src.iovlen > static_cast<unsigned>(std::numeric_limits<int>::max())) {
        return false;
    }
    dst.iov_count = static_cast<int>(src.iovlen);
    dst.ecn = src.ecn >= 0 ? to_udp_ecn(src.ecn) : fiber::net::UdpEcn::Unspecified;
    if (!src.local_sa) {
        return true;
    }
    if (!to_socket_address(src.local_sa, dst.local)) {
        return false;
    }
    dst.has_local = true;
    return true;
}

int packets_out_cb(void *ctx, const struct lsquic_out_spec *specs, unsigned n_specs) {
    auto *server = static_cast<ServerCtx *>(ctx);
    if (!server || !server->udp) {
        return -1;
    }

    unsigned sent = 0;
    for (unsigned i = 0; i < n_specs; ++i) {
        fiber::net::UdpPacketSendSpec spec{};
        if (!fill_send_spec(specs[i], spec)) {
            break;
        }

        auto result = server->udp->try_send_packet(spec);
        if (result) {
            ++sent;
            continue;
        }
        if (result.error() == IoErr::MessageTooLarge) {
            continue;
        }
        if (result.error() == IoErr::WouldBlock) {
            server->tx_blocked = true;
        }
        break;
    }
    return static_cast<int>(sent);
}

SSL_CTX *get_ssl_ctx_cb(void *peer_ctx, const sockaddr *local) {
    (void) local;
    auto *server = static_cast<ServerCtx *>(peer_ctx);
    if (server) {
        return server->ssl_ctx;
    }
    return g_server ? g_server->ssl_ctx : nullptr;
}

lsquic_conn_ctx_t *on_new_conn(void *stream_if_ctx, lsquic_conn_t *conn) {
    (void) stream_if_ctx;
    (void) conn;
    return reinterpret_cast<lsquic_conn_ctx_t *>(new ConnCtx());
}

void on_conn_closed(lsquic_conn_t *conn) {
    auto *ctx = reinterpret_cast<ConnCtx *>(lsquic_conn_get_ctx(conn));
    lsquic_conn_set_ctx(conn, nullptr);
    delete ctx;
}

lsquic_stream_ctx_t *on_new_stream(void *stream_if_ctx, lsquic_stream_t *stream) {
    (void) stream_if_ctx;
    auto *ctx = new StreamCtx();
    lsquic_stream_wantread(stream, 1);
    return reinterpret_cast<lsquic_stream_ctx_t *>(ctx);
}

void on_read(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx) {
        lsquic_stream_close(stream);
        return;
    }

    std::array<char, 4096> buf{};
    for (;;) {
        ssize_t nr = lsquic_stream_read(stream, buf.data(), buf.size());
        if (nr > 0) {
            ctx->request.append(buf.data(), static_cast<size_t>(nr));
            if (ctx->request.find("\r\n\r\n") != std::string::npos) {
                ctx->ready_to_respond = true;
                lsquic_stream_wantread(stream, 0);
                lsquic_stream_wantwrite(stream, 1);
                return;
            }
            continue;
        }
        if (nr == 0) {
            ctx->ready_to_respond = true;
            lsquic_stream_wantread(stream, 0);
            lsquic_stream_wantwrite(stream, 1);
            return;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            return;
        }
        lsquic_stream_close(stream);
        return;
    }
}

void on_write(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    if (!ctx || !ctx->ready_to_respond) {
        return;
    }

    if (!ctx->headers_sent) {
        HeaderBuf header_buf{};
        std::array<lsxpack_header, 3> headers_arr{};
        std::string content_len = std::to_string(kBody.size());
        if (0 != header_set_ptr(&headers_arr[0], header_buf, ":status", "200") ||
            0 != header_set_ptr(&headers_arr[1], header_buf, "content-type", "text/plain") ||
            0 != header_set_ptr(&headers_arr[2], header_buf, "content-length", content_len)) {
            lsquic_stream_close(stream);
            return;
        }

        lsquic_http_headers headers{};
        headers.count = static_cast<int>(headers_arr.size());
        headers.headers = headers_arr.data();
        if (0 != lsquic_stream_send_headers(stream, &headers, 0)) {
            lsquic_stream_close(stream);
            return;
        }
        ctx->headers_sent = true;
    }

    while (ctx->body_offset < kBody.size()) {
        const char *ptr = kBody.data() + ctx->body_offset;
        size_t left = kBody.size() - ctx->body_offset;
        ssize_t nw = lsquic_stream_write(stream, ptr, left);
        if (nw > 0) {
            ctx->body_offset += static_cast<size_t>(nw);
            continue;
        }
        if (nw == 0 || errno == EWOULDBLOCK || errno == EAGAIN) {
            return;
        }
        lsquic_stream_close(stream);
        return;
    }

    lsquic_stream_shutdown(stream, 1);
    lsquic_stream_wantwrite(stream, 0);
}

void on_close(lsquic_stream_t *stream, lsquic_stream_ctx_t *h) {
    (void) stream;
    auto *ctx = reinterpret_cast<StreamCtx *>(h);
    delete ctx;
}

lsquic_stream_if make_stream_if() {
    lsquic_stream_if iface{};
    iface.on_new_conn = on_new_conn;
    iface.on_conn_closed = on_conn_closed;
    iface.on_new_stream = on_new_stream;
    iface.on_read = on_read;
    iface.on_write = on_write;
    iface.on_close = on_close;
    return iface;
}

const lsquic_stream_if kStreamIf = make_stream_if();

void drain_udp_and_feed_engine(ServerCtx &server) {
    std::array<unsigned char, kMaxDatagram> packet{};

    for (;;) {
        auto recv_result = server.udp->try_recv_packet(packet.data(), packet.size());
        if (!recv_result) {
            if (recv_result.error() == IoErr::WouldBlock) {
                break;
            }
            if (recv_result.error() == IoErr::Interrupted) {
                continue;
            }
            break;
        }
        if (recv_result->size == 0) {
            continue;
        }

        recv_result->peer = normalize_socket_address(recv_result->peer);
        recv_result->local = normalize_socket_address(recv_result->local);

        sockaddr_storage local{};
        sockaddr_storage peer{};
        socklen_t local_len = 0;
        socklen_t peer_len = 0;
        if (!recv_result->local.to_sockaddr(local, local_len) || !recv_result->peer.to_sockaddr(peer, peer_len)) {
            continue;
        }

        lsquic_engine_packet_in(server.engine,
                                packet.data(),
                                recv_result->size,
                                reinterpret_cast<const sockaddr *>(&local),
                                reinterpret_cast<const sockaddr *>(&peer),
                                &server,
                                to_lsquic_ecn(recv_result->ecn));
    }
}

int compute_wait_timeout_ms(lsquic_engine_t *engine, bool tx_blocked) {
    int timeout_ms = -1;
    int diff_us = 0;
    if (lsquic_engine_earliest_adv_tick(engine, &diff_us)) {
        timeout_ms = diff_us <= 0 ? 0 : static_cast<int>((diff_us + 999) / 1000);
    }
    if (tx_blocked && (timeout_ms < 0 || timeout_ms > 5)) {
        timeout_ms = 5;
    }
    if (timeout_ms < 0 || timeout_ms > static_cast<int>(kStopPollInterval.count())) {
        timeout_ms = static_cast<int>(kStopPollInterval.count());
    }
    return timeout_ms;
}

fiber::async::Task<fiber::common::IoResult<IoEvent>> wait_for_udp_events(ServerCtx &server) {
    IoEvent interested = IoEvent::Read;
    if (server.tx_blocked) {
        interested |= IoEvent::Write;
    }
    int timeout_ms = compute_wait_timeout_ms(server.engine, server.tx_blocked);
    co_return co_await server.udp->wait_event(interested, std::chrono::milliseconds(timeout_ms));
}

DetachedTask run_server(ServerCtx *server) {
    if (!server || !server->loop || !server->udp || !server->engine) {
        co_return;
    }

    while (!server->stop.load(std::memory_order_acquire)) {
        auto wait_result = co_await wait_for_udp_events(*server);
        if (wait_result) {
            if (fiber::event::any(*wait_result & IoEvent::Read)) {
                drain_udp_and_feed_engine(*server);
            }
            if (fiber::event::any(*wait_result & IoEvent::Write)) {
                server->tx_blocked = false;
                lsquic_engine_send_unsent_packets(server->engine);
            }
        } else if (wait_result.error() != IoErr::TimedOut) {
            break;
        }

        lsquic_engine_process_conns(server->engine);
        if (!server->tx_blocked && lsquic_engine_has_unsent_packets(server->engine)) {
            lsquic_engine_send_unsent_packets(server->engine);
        }
        if (lsquic_engine_has_unsent_packets(server->engine)) {
            server->tx_blocked = true;
        }
    }

    if (server->udp && server->udp->valid()) {
        server->udp->close();
    }
    server->loop->stop();
    co_return;
}

void on_signal(int signo) {
    (void) signo;
    if (g_server) {
        g_server->stop.store(true, std::memory_order_release);
    }
}

bool install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
        return false;
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
        return false;
    }
    return true;
}

SSL_CTX *create_server_ssl_ctx(const char *cert_file, const char *key_file) {
    SSL_CTX *ssl = SSL_CTX_new(TLS_method());
    if (!ssl) {
        return nullptr;
    }

    SSL_CTX_set_min_proto_version(ssl, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ssl, TLS1_3_VERSION);
    SSL_CTX_set_alpn_select_cb(ssl, select_alpn_cb, nullptr);
    if (SSL_CTX_use_certificate_chain_file(ssl, cert_file) != 1) {
        SSL_CTX_free(ssl);
        return nullptr;
    }
    if (SSL_CTX_use_PrivateKey_file(ssl, key_file, SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ssl);
        return nullptr;
    }
    if (SSL_CTX_check_private_key(ssl) != 1) {
        SSL_CTX_free(ssl);
        return nullptr;
    }
    return ssl;
}

lsquic_engine_t *create_engine(ServerCtx &server) {
    lsquic_engine_init_settings(&server.settings, LSENG_SERVER | LSENG_HTTP);
    server.settings.es_support_srej = 0;

    lsquic_engine_api api{};
    api.ea_packets_out = packets_out_cb;
    api.ea_packets_out_ctx = &server;
    api.ea_stream_if = &kStreamIf;
    api.ea_stream_if_ctx = &server;
    api.ea_settings = &server.settings;
    api.ea_get_ssl_ctx = get_ssl_ctx_cb;

    return lsquic_engine_new(LSENG_SERVER | LSENG_HTTP, &api);
}

} // namespace

int main(int argc, char **argv) {
    uint16_t port = 8443;
    const char *cert_file = nullptr;
    const char *key_file = nullptr;
    if (argc == 3) {
        cert_file = argv[1];
        key_file = argv[2];
    } else if (argc == 4) {
        if (!parse_port(argv[1], port)) {
            std::cerr << "invalid port: " << argv[1] << '\n';
            return 1;
        }
        cert_file = argv[2];
        key_file = argv[3];
    } else {
        std::cerr << "usage: http3_demo_lsquic [port] <cert.pem> <key.pem>\n";
        return 1;
    }

    fiber::event::EventLoop loop;
    fiber::net::UdpSocket udp(loop);
    ServerCtx server{};
    server.loop = &loop;
    server.udp = &udp;
    server.port = port;
    g_server = &server;

    if (!install_signal_handlers()) {
        std::cerr << "failed to install signal handlers\n";
        g_server = nullptr;
        return 1;
    }
    configure_lsquic_logging();

    if (!bind_udp_socket(udp, server.port)) {
        std::cerr << "udp bind failed\n";
        g_server = nullptr;
        return 1;
    }
    server.port = udp.local_addr().port();

    if (lsquic_global_init(LSQUIC_GLOBAL_SERVER) != 0) {
        std::cerr << "lsquic_global_init failed\n";
        g_server = nullptr;
        return 1;
    }

    server.ssl_ctx = create_server_ssl_ctx(cert_file, key_file);
    if (!server.ssl_ctx) {
        std::cerr << "SSL_CTX init failed\n";
        lsquic_global_cleanup();
        g_server = nullptr;
        return 1;
    }

    server.engine = create_engine(server);
    if (!server.engine) {
        std::cerr << "lsquic_engine_new failed\n";
        SSL_CTX_free(server.ssl_ctx);
        lsquic_global_cleanup();
        g_server = nullptr;
        return 1;
    }

    std::cout << "listening on udp://[::]:" << server.port << '\n';
    std::cout << "try: curl --http3 -k https://127.0.0.1:" << server.port << "/\n";

    fiber::async::spawn(loop, [&]() { return run_server(&server); });
    loop.run();

    lsquic_engine_destroy(server.engine);
    SSL_CTX_free(server.ssl_ctx);
    lsquic_global_cleanup();
    g_server = nullptr;
    return 0;
}
