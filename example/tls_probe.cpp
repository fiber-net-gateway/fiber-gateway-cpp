// tls_probe: TLS interop probe over the public net API (TlsTcpStream →
// in-tree fiber::tls engines). scripts/interop/openssl_matrix.sh drives it
// against `openssl s_server` (client mode) and `openssl s_client` (server
// mode).
//
// Client mode (--connect): connects, handshakes, writes one request, prints
// the response until the peer closes, and reports on stderr:
//   PROBE handshake=ok alpn=<proto|-> bytes=<n> close=<clean|timeout|error:<err>>
//   PROBE handshake=fail err=<IoErr>
// Exit status: 0 = handshake ok and response read to a clean close,
// 2 = handshake failed, 3 = I/O error after the handshake, 1 = usage/setup.
//
// Server mode (--listen): serves --accept N connections one at a time:
// handshake, read one chunk, reply "PROBE-ECHO " + chunk, close_notify.
// Per connection on stderr:
//   SERVE handshake=ok alpn=<proto|-> bytes=<n>
//   SERVE handshake=fail err=<IoErr>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoop.h>
#include <fiber/net/IpAddress.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/TlsTcpStream.h>
#include <fiber/net/TrustStore.h>

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;

struct Options {
    bool listen = false;
    int accept = 1; // server mode: connections to serve
    bool require_client_cert = false; // server mode, with --ca
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    std::string sni;
    std::string verify_name;
    std::string ca_file; // non-empty: verify the peer against this bundle only
    std::string cert_file; // client certificate (mTLS)
    std::string key_file;
    std::vector<std::string> alpn;
    int min_version = 0x0303;
    int max_version = 0x0304;
    std::string request = "GET / HTTP/1.0\r\n\r\n";
    std::string then_after; // once the response contains this, write `then` (once)
    std::string then;
    std::chrono::milliseconds timeout{5000};
};

void usage() {
    std::cerr << "usage: tls_probe --connect HOST:PORT [--sni NAME] [--verify-name NAME] [--ca FILE]\n"
                 "                 [--cert FILE --key FILE] [--alpn p1,p2] [--min 1.2|1.3] [--max 1.2|1.3]\n"
                 "                 [--request STR (\\r\\n escapes) | --get PATH] [--timeout-ms N]\n"
                 "                 [--then-after MARK --then STR]  (second write once MARK was read)\n"
                 "       tls_probe --listen PORT --cert FILE --key FILE [--accept N] [--alpn p1,p2]\n"
                 "                 [--ca FILE [--require-client-cert 1]] [--min 1.2|1.3] [--max 1.2|1.3]\n";
}

std::optional<int> parse_version(std::string_view v) {
    if (v == "1.2") {
        return 0x0303;
    }
    if (v == "1.3") {
        return 0x0304;
    }
    return std::nullopt;
}

std::string unescape(std::string_view in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            const char c = in[++i];
            out.push_back(c == 'r' ? '\r' : c == 'n' ? '\n' : c);
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::optional<Options> parse_args(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) {
            return std::nullopt;
        }
        const std::string_view val = argv[++i];
        if (arg == "--connect") {
            const auto colon = val.rfind(':');
            if (colon == std::string_view::npos) {
                return std::nullopt;
            }
            o.host = std::string(val.substr(0, colon));
            o.port = static_cast<std::uint16_t>(std::atoi(std::string(val.substr(colon + 1)).c_str()));
        } else if (arg == "--listen") {
            o.listen = true;
            o.port = static_cast<std::uint16_t>(std::atoi(std::string(val).c_str()));
        } else if (arg == "--accept") {
            o.accept = std::atoi(std::string(val).c_str());
        } else if (arg == "--require-client-cert") {
            o.require_client_cert = val == "1";
        } else if (arg == "--sni") {
            o.sni = val;
        } else if (arg == "--verify-name") {
            o.verify_name = val;
        } else if (arg == "--ca") {
            o.ca_file = val;
        } else if (arg == "--cert") {
            o.cert_file = val;
        } else if (arg == "--key") {
            o.key_file = val;
        } else if (arg == "--alpn") {
            std::size_t start = 0;
            while (start <= val.size()) {
                const auto comma = val.find(',', start);
                const auto end = comma == std::string_view::npos ? val.size() : comma;
                if (end > start) {
                    o.alpn.emplace_back(val.substr(start, end - start));
                }
                start = end + 1;
            }
        } else if (arg == "--min" || arg == "--max") {
            const auto v = parse_version(val);
            if (!v) {
                return std::nullopt;
            }
            (arg == "--min" ? o.min_version : o.max_version) = *v;
        } else if (arg == "--request") {
            o.request = unescape(val);
        } else if (arg == "--then-after") {
            o.then_after = val;
        } else if (arg == "--then") {
            o.then = unescape(val);
        } else if (arg == "--get") {
            o.request = "GET " + std::string(val) + " HTTP/1.0\r\n\r\n";
        } else if (arg == "--timeout-ms") {
            o.timeout = std::chrono::milliseconds(std::atoi(std::string(val).c_str()));
        } else {
            return std::nullopt;
        }
    }
    if (o.port == 0 || (o.cert_file.empty() != o.key_file.empty()) || (o.listen && o.cert_file.empty())) {
        return std::nullopt;
    }
    return o;
}

// Blocking connect (a probe, not a request path), then non-blocking for the loop.
int connect_blocking(const Options &o, fiber::net::SocketAddress &peer) {
    fiber::net::IpAddress ip;
    if (!fiber::net::IpAddress::parse(o.host, ip)) {
        std::cerr << "bad host (IP literal expected): " << o.host << '\n';
        return -1;
    }
    peer = fiber::net::SocketAddress(ip, o.port);
    sockaddr_storage ss{};
    socklen_t len = 0;
    if (ip.is_v4()) {
        auto *sin = reinterpret_cast<sockaddr_in *>(&ss);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(o.port);
        inet_pton(AF_INET, o.host.c_str(), &sin->sin_addr);
        len = sizeof(sockaddr_in);
    } else {
        auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(o.port);
        inet_pton(AF_INET6, o.host.c_str(), &sin6->sin6_addr);
        len = sizeof(sockaddr_in6);
    }
    const int fd = ::socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&ss), len) != 0) {
        std::cerr << "connect failed: " << std::strerror(errno) << '\n';
        ::close(fd);
        return -1;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

DetachedTask run_probe(fiber::event::EventLoop *loop, const Options *o, int *exit_code) {
    // Setup failures stop the loop with exit 1.
    const auto fail_setup = [&](const char *what) {
        std::cerr << what << '\n';
        *exit_code = 1;
        loop->stop();
    };

    std::unique_ptr<fiber::net::TrustStore> trust;
    if (!o->ca_file.empty()) {
        auto created = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(o->ca_file));
        if (!created) {
            fail_setup("trust store load failed");
            co_return;
        }
        trust = std::move(*created);
    }
    fiber::net::TlsCredential credential;
    if (!o->cert_file.empty()) {
        fiber::net::TlsCredentialOptions copts;
        copts.certificate_chain = fiber::net::TlsPemSource::from_file(o->cert_file);
        copts.private_key = fiber::net::TlsPemSource::from_file(o->key_file);
        auto created = fiber::net::TlsCredential::create(copts);
        if (!created) {
            fail_setup("client credential load failed");
            co_return;
        }
        credential = std::move(*created);
    }

    fiber::net::SocketAddress peer;
    const int fd = connect_blocking(*o, peer);
    if (fd < 0) {
        fail_setup("tcp connect failed");
        co_return;
    }
    fiber::net::TlsTcpStream stream(*loop, fd, peer);

    std::vector<std::string_view> alpn_views(o->alpn.begin(), o->alpn.end());
    fiber::net::TlsClientParam param;
    param.security.trust_store = trust.get();
    param.security.verify_peer = trust != nullptr;
    param.security.credential = credential.empty() ? nullptr : &credential;
    param.min_version = o->min_version;
    param.max_version = o->max_version;
    param.alpn = alpn_views;
    param.server_name = o->sni;
    param.verify_name = o->verify_name;

    auto handshake = co_await stream.handshake(param, o->timeout);
    if (!handshake) {
        std::cerr << "PROBE handshake=fail err=" << fiber::common::io_err_name(handshake.error()) << '\n';
        *exit_code = 2;
        loop->stop();
        co_return;
    }
    const std::string_view alpn = stream.selected_alpn();

    fiber::mem::IoBufChain request;
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(o->request.size());
    std::memcpy(buf.writable_data(), o->request.data(), o->request.size());
    buf.commit(o->request.size());
    (void) request.append(std::move(buf));
    while (request.readable_bytes() > 0) {
        auto wrote = co_await stream.writev(request, o->timeout);
        if (!wrote) {
            std::cerr << "PROBE handshake=ok alpn=" << (alpn.empty() ? "-" : alpn)
                      << " bytes=0 close=error:" << fiber::common::io_err_name(wrote.error()) << '\n';
            *exit_code = 3;
            loop->stop();
            co_return;
        }
    }

    std::size_t total = 0;
    std::string close = "clean";
    std::string seen; // response so far, only while a --then write is pending
    bool then_pending = !o->then.empty() && !o->then_after.empty();
    for (;;) {
        fiber::mem::IoBufChain in;
        auto got = co_await stream.readv(64 * 1024, in, o->timeout);
        if (!got) {
            close = got.error() == IoErr::TimedOut
                            ? "timeout"
                            : std::string("error:") + std::string(fiber::common::io_err_name(got.error()));
            break;
        }
        if (*got == 0) {
            break; // close_notify
        }
        total += *got;
        while (const fiber::mem::IoBuf *front = in.first_readable()) {
            std::cout.write(reinterpret_cast<const char *>(front->readable_data()),
                            static_cast<std::streamsize>(front->readable()));
            if (then_pending) {
                seen.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
            }
            in.consume_and_compact(front->readable());
        }
        if (then_pending && seen.find(o->then_after) != std::string::npos) {
            // A second write mid-stream: after a peer KeyUpdate(update_requested)
            // this is where our own KeyUpdate must precede the data.
            then_pending = false;
            fiber::mem::IoBufChain more;
            fiber::mem::IoBuf chunk = fiber::mem::IoBuf::allocate(o->then.size());
            std::memcpy(chunk.writable_data(), o->then.data(), o->then.size());
            chunk.commit(o->then.size());
            (void) more.append(std::move(chunk));
            while (more.readable_bytes() > 0) {
                auto wrote = co_await stream.writev(more, o->timeout);
                if (!wrote) {
                    close = std::string("error:") + std::string(fiber::common::io_err_name(wrote.error()));
                    break;
                }
            }
        }
    }
    std::cout.flush();
    std::cerr << "PROBE handshake=ok alpn=" << (alpn.empty() ? "-" : alpn) << " bytes=" << total << " close=" << close
              << '\n';
    *exit_code = close == "clean" ? 0 : 3;
    stream.close();
    loop->stop();
    co_return;
}

// Server mode: one connection at a time keeps stderr lines in accept order.
DetachedTask run_server(fiber::event::EventLoop *loop, const Options *o, int *exit_code) {
    const auto fail_setup = [&](const char *what) {
        std::cerr << what << '\n';
        *exit_code = 1;
        loop->stop();
    };
    fiber::net::TlsCredentialOptions copts;
    copts.certificate_chain = fiber::net::TlsPemSource::from_file(o->cert_file);
    copts.private_key = fiber::net::TlsPemSource::from_file(o->key_file);
    auto credential = fiber::net::TlsCredential::create(copts);
    if (!credential) {
        fail_setup("server credential load failed");
        co_return;
    }
    std::unique_ptr<fiber::net::TrustStore> trust;
    if (!o->ca_file.empty()) {
        auto created = fiber::net::TrustStore::create(fiber::net::TrustStoreOptions::from_file(o->ca_file));
        if (!created) {
            fail_setup("trust store load failed");
            co_return;
        }
        trust = std::move(*created);
    }

    fiber::net::TcpListener listener(*loop);
    if (!listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), o->port), {})) {
        fail_setup("bind failed");
        co_return;
    }
    std::cerr << "SERVE listening port=" << o->port << std::endl;

    std::vector<std::string_view> alpn_views(o->alpn.begin(), o->alpn.end());
    fiber::net::TlsServerParam param;
    param.configure_callback = fiber::net::configure_tls_with_credential;
    param.configure_ctx = &*credential;
    param.trust_store = trust.get();
    param.client_certificate_mode = trust == nullptr         ? fiber::net::TlsClientCertificateMode::None
                                    : o->require_client_cert ? fiber::net::TlsClientCertificateMode::Required
                                                             : fiber::net::TlsClientCertificateMode::Optional;
    param.alpn = alpn_views;
    param.min_version = o->min_version;
    param.max_version = o->max_version;

    *exit_code = 0;
    for (int served = 0; served < o->accept; ++served) {
        auto accepted = co_await listener.accept();
        if (!accepted || !accepted->valid()) {
            fail_setup("accept failed");
            co_return;
        }
        auto accept = std::move(*accepted);
        fiber::net::TlsTcpStream stream(*loop, accept.release_fd(), accept.take_peer());
        auto handshake = co_await stream.handshake(param, o->timeout);
        if (!handshake) {
            std::cerr << "SERVE handshake=fail err=" << fiber::common::io_err_name(handshake.error()) << std::endl;
            *exit_code = 2;
            continue;
        }
        const std::string_view alpn = stream.selected_alpn();
        fiber::mem::IoBufChain in;
        auto got = co_await stream.readv(64 * 1024, in, o->timeout);
        std::string reply = "PROBE-ECHO ";
        std::size_t bytes = got ? *got : 0;
        while (const fiber::mem::IoBuf *front = in.first_readable()) {
            reply.append(reinterpret_cast<const char *>(front->readable_data()), front->readable());
            in.consume_and_compact(front->readable());
        }
        fiber::mem::IoBufChain out;
        fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(reply.size());
        std::memcpy(buf.writable_data(), reply.data(), reply.size());
        buf.commit(reply.size());
        (void) out.append(std::move(buf));
        while (out.readable_bytes() > 0) {
            if (!co_await stream.writev(out, o->timeout)) {
                break;
            }
        }
        std::cerr << "SERVE handshake=ok alpn=" << (alpn.empty() ? "-" : alpn) << " bytes=" << bytes << std::endl;
        stream.close(); // close_notify, then the fd
    }
    loop->stop();
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    const auto options = parse_args(argc, argv);
    if (!options) {
        usage();
        return 1;
    }
    fiber::event::EventLoop loop;
    int exit_code = 1;
    if (options->listen) {
        fiber::async::spawn(loop, [&]() { return run_server(&loop, &*options, &exit_code); });
    } else {
        fiber::async::spawn(loop, [&]() { return run_probe(&loop, &*options, &exit_code); });
    }
    loop.run();
    return exit_code;
}
