// TLS transport A/B benchmark: echoes a byte total over http::TlsTransport
// (in-tree engines on both ends) and reports client-observed throughput.
// Written against the HttpTransport chain API so the same source builds
// against both sides of the TlsStreamFd chain-I/O migration.
//
// usage: tls_ab_bench <mode> [total_bytes] [runs]
//   modes: bulk64k (64KB nodes)  bulk16k (16KB nodes)
//          h2 (9B header + 16375B payload)  small1k  tiny64

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TcpSocketOptions.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsParams.h>
#include <fiber/net/TlsServerHandshakeConfig.h>

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

constexpr std::size_t kReadCap = 64 * 1024;
constexpr auto kInfinite = std::chrono::milliseconds::max();

const char kSelfSignedCertPem[] = R"(-----BEGIN CERTIFICATE-----
MIIDCTCCAfGgAwIBAgIUEDCdxH6aX38+fEeFx3nlY3pJwdkwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDExNzEzMDcwNVoXDTI3MDEx
NzEzMDcwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEA4+tN+7EU3WmwFfjE4bn720reQJkTnAOUOYXg9zejQ75q
vHOpFxLU9z866mVpT7jVYAupmKfXrJ9U5Vd9znrWFzZt9rTdg+hISdujXjaEfEf+
GQ+66xthO2tAF3c6XokoqRpJR0GVInJoWaHBpV0PcvRb9AhRfuk+ja3W1dfdHnE8
LWutJCVK0HOWifIBGqpED3YMBNKZxFSKTCKLiqbxmnd6TT1fh8UI+AibEKhuJX4A
m3enMonO1PHeSOUY1dfXpZfdRdnYgjiyVyEw7oQL11r6O2LJZMJsoW912uIUnYrs
A4bDbMMfDgHe+PiyERCG62xydAlj1phGVlbGI/8HOQIDAQABo1MwUTAdBgNVHQ4E
FgQUvM4+Ad+L+GYd6i4nZgRFaPkRo7UwHwYDVR0jBBgwFoAUvM4+Ad+L+GYd6i4n
ZgRFaPkRo7UwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAxo8i
jbyceTsjxiMDoXd/OPtPCD2CcpWOUxMb4hdGk3pMK6xFq8c7bdMcn6oZMF7xpdHg
jDTrfa8TlPITcG/34MtvPS3hq7klCPi948Z9wbtJWGfKAl3rHYK7PIIj3wNipTcQ
IkfIlO/t6VKPSx1S9HQA6nCDOvCufOL54Mfz0vI9Y47c4O1TNtbJiiWUkP/pEjEw
RMeULfoobqmMYTjbjQ8nKC25cQAmhQ0koOqJPquPtAHvaowqBT6jDLEL+8vR4Kfc
9UqEtfRr0+7LgbcofOsseDFYMPBW2GdpPMJ2PMYsQtFMXRoomlhjdpIct6e3rRnd
GiDzEZ0VwkYlJDwF4w==
-----END CERTIFICATE-----
)";

const char kSelfSignedKeyPem[] = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDj6037sRTdabAV
+MThufvbSt5AmROcA5Q5heD3N6NDvmq8c6kXEtT3PzrqZWlPuNVgC6mYp9esn1Tl
V33OetYXNm32tN2D6EhJ26NeNoR8R/4ZD7rrG2E7a0AXdzpeiSipGklHQZUicmhZ
ocGlXQ9y9Fv0CFF+6T6NrdbV190ecTwta60kJUrQc5aJ8gEaqkQPdgwE0pnEVIpM
IouKpvGad3pNPV+HxQj4CJsQqG4lfgCbd6cyic7U8d5I5RjV19ell91F2diCOLJX
ITDuhAvXWvo7Yslkwmyhb3Xa4hSdiuwDhsNswx8OAd74+LIREIbrbHJ0CWPWmEZW
VsYj/wc5AgMBAAECggEAHomvmDKg1g3MHxWG46u0uCwu3T7lZrkACjkK7HTS9ke0
K23f0Qyf5kTdkvxlgN4GEOlfHuoWNrXefSAc5iaFOvT7BNw09fCQhvzbxcrOM4y9
2gPGiqvPelOjccFy26nK/eVcviRmZAgqPSA0PwDaCg/9phPbP4Lm87rAF0TmBqbq
n5s+7MXf4iFTbRIec2zTikWfbUglhNmKr3eC/4+K+hk3TX95Wltvz6dGz+godV/L
FilwLEa+e0cSTUA8FYzYtoEUiV7/8dl8VBIvQWtx8sRNNihCmnlYrJ3N8tw/hO6F
PKpfoOo+L9uRJG4LGtAkM0Pqs9U9uN5v7F5HNMxO1QKBgQD61LhiF/ftPlTRFQm2
CrnIN4PcQtIDRar/cuwgyq3F8AAfJ5PSYD/GvitaQYxa9Ya1IM3T7UPx6L3OmJl6
updR3Mh/+6BtAYwSwoWLv0tHQ01xOe9pwML52JShVocVXQFE/UXNtuffuUpXVeWk
miVen8SI4CHLeFU+6Dfcp0l3owKBgQDonbYbB9bRVzG0gbgdp2K1pxvMQizR8IkU
GsYaT/LMooBpRBOHrane+9KCztkghjmTyDKEl7jwt65fvFl0ttkipq1ISTepV6Rt
Cmdc5PnBc+ON49/6ivTGFAdU5CY3sE/7L6ngPqZq6bq8nBJ0NPcjpfEl2JfBeND8
NisrSQEjcwKBgQDlcp1QLji/LtuLf0Eo41rbCd13KTDPiXVIw6m4vW6EuGyEE0In
mZ/9f4xMvdVUh3C4U8+04z/aFFs8l18eY310hxBp8pXn4RhvOL3M/iowgCJhRuv4
wzoYLsSXaX2cTz2QDFdEPOKTRv34Mj0le1Rf4Kp5wv1nESZ5qxceo3CTHQKBgFWb
jSR/ixB57YIH53GKY6qEuJdAl2wgAOLUQ6n1WF71Qxr6gdGCGS1GMiAP7hqpK1F2
8RiZGegFQXhcQfPRQzIcc1NSFtkMtyemF4o5fq0ycEGM5qY3M4QeZOBaIrKGAblo
vjUX+XkJUb8OFUCNKZMGBCywfJEoXIklilegw3l/AoGBALtmVrX28WQ42DOYWdKD
dmDMBg1+21d8wIWs4k5bu1LdlY8XqMnV9TAHwOwGcleK2uM3AfoLOho6HwFwdyhJ
x20XBogOziImjh+cvWNpm951EC3oWHOFYPsMjX1mRCye88LQHwm3gQ8iCIOzPj+8
RB6SahiCZEhAtLq/9Q/O1bL5
-----END PRIVATE KEY-----
)";

std::string make_temp_path(const char *tag) {
    std::string path = "/tmp/fiber_tls_ab_bench_";
    path.append(tag);
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::getpid())));
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::random())));
    path.append(".pem");
    return path;
}

bool write_file(const std::string &path, std::string_view data) {
    FILE *out = ::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        return false;
    }
    const bool ok = ::fwrite(data.data(), 1, data.size(), out) == data.size();
    ::fclose(out);
    return ok;
}

struct TempFile {
    std::string path;

    explicit TempFile(const char *tag, std::string_view data) : path(make_temp_path(tag)) {
        if (!write_file(path, data)) {
            path.clear();
        }
    }

    ~TempFile() {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
};

struct BenchPair {
    std::unique_ptr<fiber::net::TlsCredential> server_credential;
    fiber::net::TlsServerParam server_param;
    fiber::net::TlsClientParam client_param;
};

fiber::common::IoResult<BenchPair> make_bench_pair(const std::string &cert_path, const std::string &key_path) {
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(cert_path);
    credential_options.private_key = fiber::net::TlsPemSource::from_file(key_path);
    auto credential = fiber::net::TlsCredential::create(credential_options);
    if (!credential) {
        return std::unexpected(credential.error());
    }
    BenchPair pair{};
    pair.server_credential = std::move(*credential);
    pair.server_param.configure_callback = &fiber::net::configure_tls_with_credential;
    pair.server_param.configure_ctx = pair.server_credential.get();
    return pair;
}

struct Workload {
    const char *name;
    std::size_t node_a; // leading node bytes (0 = none)
    std::size_t node_b; // trailing node bytes (0 = none)
    std::uint64_t default_total;
};

const Workload kWorkloads[] = {
        {"bulk64k", 65536, 0, 256ULL << 20}, {"bulk16k", 16384, 0, 256ULL << 20}, {"h2", 9, 16375, 128ULL << 20},
        {"small1k", 1024, 0, 64ULL << 20},   {"tiny64", 64, 0, 16ULL << 20},
};

std::size_t group_bytes(const Workload &wl) { return wl.node_a + wl.node_b; }

bool build_write_chain(fiber::mem::IoBufChain &chain, const Workload &wl, const std::vector<std::uint8_t> &pattern) {
    for (const std::size_t len: {wl.node_a, wl.node_b}) {
        if (len == 0) {
            continue;
        }
        fiber::mem::IoBuf node = fiber::mem::IoBuf::allocate(len);
        if (!node.valid()) {
            return false;
        }
        std::memcpy(node.writable_data(), pattern.data(), len);
        node.commit(len);
        if (!chain.append(std::move(node))) {
            return false;
        }
    }
    return true;
}

// A blocking loopback TCP connection set up on the main thread; the fds are
// handed to the two event loops as accepted/connected transports.
struct LoopbackPair {
    int client_fd = -1;
    int server_fd = -1;
    fiber::net::SocketAddress client_peer;
    fiber::net::SocketAddress server_peer;
};

bool make_loopback_pair(LoopbackPair &out) {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd, 1) != 0) {
        ::close(listen_fd);
        return false;
    }
    sockaddr_storage bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&bound), &bound_len) != 0) {
        ::close(listen_fd);
        return false;
    }
    const int client_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client_fd < 0 || ::connect(client_fd, reinterpret_cast<const sockaddr *>(&bound), bound_len) != 0) {
        ::close(listen_fd);
        if (client_fd >= 0) {
            ::close(client_fd);
        }
        return false;
    }
    const int server_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
    ::close(listen_fd);
    if (server_fd < 0) {
        ::close(client_fd);
        return false;
    }
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    if (::getpeername(server_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len) != 0 ||
        !fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&peer), peer_len,
                                                  out.client_peer)) {
        ::close(client_fd);
        ::close(server_fd);
        return false;
    }
    peer_len = sizeof(peer);
    if (::getpeername(client_fd, reinterpret_cast<sockaddr *>(&peer), &peer_len) != 0 ||
        !fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<const sockaddr *>(&peer), peer_len,
                                                  out.server_peer)) {
        ::close(client_fd);
        ::close(server_fd);
        return false;
    }
    out.client_fd = client_fd;
    out.server_fd = server_fd;
    return true;
}

fiber::async::Task<fiber::common::IoErr> echo_phase(fiber::http::TlsTransport &transport, std::uint64_t total) {
    std::uint64_t received = 0;
    while (received < total) {
        fiber::mem::IoBufChain in;
        auto read_result = co_await transport.readv(kReadCap, in, kInfinite);
        if (!read_result) {
            co_return read_result.error();
        }
        if (*read_result == 0) {
            break; // client closed early
        }
        received += *read_result;
        while (in.readable_bytes() > 0) {
            auto write_result = co_await transport.writev(in, kInfinite);
            if (!write_result) {
                co_return write_result.error();
            }
        }
    }
    co_return fiber::common::IoErr::None;
}

DetachedTask server_run(fiber::event::EventLoop &loop, fiber::net::AcceptResult accept,
                        fiber::net::TlsServerParam param, std::uint64_t total,
                        std::promise<fiber::common::IoErr> *done) {
    auto transport = fiber::http::TlsTransport::create(loop, std::move(accept), fiber::net::kNoDelayTcpSocketOptions);
    if (!transport) {
        done->set_value(transport.error());
        co_return;
    }
    auto handshake_result = co_await (*transport)->handshake(param);
    if (!handshake_result) {
        done->set_value(handshake_result.error());
        co_return;
    }
    const fiber::common::IoErr err = co_await echo_phase(**transport, total);
    (*transport)->close();
    done->set_value(err);
    co_return;
}

DetachedTask client_handshake(fiber::event::EventLoop &loop, fiber::net::AcceptResult accept,
                              fiber::net::TlsClientParam param, fiber::http::TlsTransport **slot,
                              std::promise<fiber::common::IoErr> *done) {
    auto transport = fiber::http::TlsTransport::create(loop, std::move(accept), fiber::net::kNoDelayTcpSocketOptions);
    if (!transport) {
        done->set_value(transport.error());
        co_return;
    }
    auto handshake_result = co_await (*transport)->handshake(param);
    if (!handshake_result) {
        done->set_value(handshake_result.error());
        co_return;
    }
    *slot = transport->release();
    done->set_value(fiber::common::IoErr::None);
    co_return;
}

DetachedTask client_writer(fiber::http::TlsTransport *transport, std::uint64_t total, const Workload &wl,
                           const std::vector<std::uint8_t> &pattern, std::promise<fiber::common::IoErr> *done) {
    std::uint64_t sent = 0;
    while (sent < total) {
        fiber::mem::IoBufChain chain;
        if (!build_write_chain(chain, wl, pattern)) {
            done->set_value(fiber::common::IoErr::NoMem);
            co_return;
        }
        while (chain.readable_bytes() > 0) {
            auto write_result = co_await transport->writev(chain, kInfinite);
            if (!write_result) {
                done->set_value(write_result.error());
                co_return;
            }
        }
        sent += group_bytes(wl);
    }
    done->set_value(fiber::common::IoErr::None);
    co_return;
}

DetachedTask client_reader(fiber::http::TlsTransport *transport, std::uint64_t total,
                           std::promise<fiber::common::IoErr> *done) {
    std::uint64_t received = 0;
    while (received < total) {
        fiber::mem::IoBufChain in;
        auto read_result = co_await transport->readv(kReadCap, in, kInfinite);
        if (!read_result) {
            done->set_value(read_result.error());
            co_return;
        }
        if (*read_result == 0) {
            done->set_value(fiber::common::IoErr::ConnReset);
            co_return;
        }
        received += *read_result;
    }
    done->set_value(fiber::common::IoErr::None);
    co_return;
}

DetachedTask client_close(fiber::http::TlsTransport *transport, std::promise<void> *done) {
    transport->close();
    delete transport;
    done->set_value();
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <mode> [total_bytes] [runs]\n", argv[0]);
        return 2;
    }
    const Workload *wl = nullptr;
    for (const Workload &candidate: kWorkloads) {
        if (std::string_view(candidate.name) == argv[1]) {
            wl = &candidate;
            break;
        }
    }
    if (wl == nullptr) {
        std::fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    std::uint64_t total = wl->default_total;
    int runs = 5;
    if (argc >= 3) {
        total = std::strtoull(argv[2], nullptr, 10);
    }
    if (argc >= 4) {
        runs = std::atoi(argv[3]);
    }

    ::signal(SIGPIPE, SIG_IGN);
    ::srandom(42);

    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    if (cert.path.empty() || key.path.empty()) {
        std::fprintf(stderr, "failed to write cert/key temp files\n");
        return 1;
    }
    auto pair = make_bench_pair(cert.path, key.path);
    if (!pair) {
        std::fprintf(stderr, "credential error: %s\n", fiber::common::io_err_name(pair.error()).data());
        return 1;
    }

    std::vector<std::uint8_t> pattern(65536);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<std::uint8_t>('a' + (i % 26));
    }

    fiber::event::EventLoopGroup group(2);
    group.start();
    fiber::event::EventLoop &server_loop = group.at(0);
    fiber::event::EventLoop &client_loop = group.at(1);

    for (int run = 1; run <= runs; ++run) {
        LoopbackPair sockets;
        if (!make_loopback_pair(sockets)) {
            std::fprintf(stderr, "run %d: loopback setup failed\n", run);
            return 1;
        }
        fiber::net::AcceptResult server_accept(sockets.server_fd, sockets.server_peer);
        fiber::net::AcceptResult client_accept(sockets.client_fd, sockets.client_peer);

        std::promise<fiber::common::IoErr> server_done;
        std::promise<fiber::common::IoErr> client_hs_done;
        fiber::http::TlsTransport *client = nullptr;
        fiber::async::spawn(server_loop, [&]() {
            return server_run(server_loop, std::move(server_accept), pair->server_param, total, &server_done);
        });
        fiber::async::spawn(client_loop, [&]() {
            return client_handshake(client_loop, std::move(client_accept), pair->client_param, &client,
                                    &client_hs_done);
        });

        auto client_hs_future = client_hs_done.get_future();
        if (client_hs_future.wait_for(10s) != std::future_status::ready) {
            std::fprintf(stderr, "run %d: client handshake timeout\n", run);
            return 1;
        }
        const fiber::common::IoErr hs_err = client_hs_future.get();
        if (hs_err != fiber::common::IoErr::None || client == nullptr) {
            std::fprintf(stderr, "run %d: client handshake failed: %s\n", run,
                         fiber::common::io_err_name(hs_err).data());
            return 1;
        }

        const auto t0 = std::chrono::steady_clock::now();
        std::promise<fiber::common::IoErr> writer_done;
        std::promise<fiber::common::IoErr> reader_done;
        fiber::async::spawn(client_loop, [&]() { return client_writer(client, total, *wl, pattern, &writer_done); });
        fiber::async::spawn(client_loop, [&]() { return client_reader(client, total, &reader_done); });

        auto writer_future = writer_done.get_future();
        auto reader_future = reader_done.get_future();
        bool ok = true;
        if (writer_future.wait_for(60s) != std::future_status::ready) {
            std::fprintf(stderr, "run %d: writer timeout\n", run);
            ok = false;
        } else if (writer_future.get() != fiber::common::IoErr::None) {
            std::fprintf(stderr, "run %d: writer failed: %s\n", run,
                         fiber::common::io_err_name(writer_future.get()).data());
            ok = false;
        }
        if (ok && reader_future.wait_for(60s) != std::future_status::ready) {
            std::fprintf(stderr, "run %d: reader timeout\n", run);
            ok = false;
        } else if (ok && reader_future.get() != fiber::common::IoErr::None) {
            std::fprintf(stderr, "run %d: reader failed\n", run);
            ok = false;
        }
        const auto t1 = std::chrono::steady_clock::now();

        auto server_future = server_done.get_future();
        if (server_future.wait_for(10s) != std::future_status::ready) {
            std::fprintf(stderr, "run %d: server timeout\n", run);
            ok = false;
        } else if (server_future.get() != fiber::common::IoErr::None) {
            std::fprintf(stderr, "run %d: server failed: %s\n", run,
                         fiber::common::io_err_name(server_future.get()).data());
            ok = false;
        }

        std::promise<void> close_done;
        auto close_future = close_done.get_future();
        fiber::async::spawn(client_loop, [&]() { return client_close(client, &close_done); });
        if (close_future.wait_for(10s) != std::future_status::ready) {
            std::fprintf(stderr, "run %d: close timeout\n", run);
            return 1;
        }

        if (!ok) {
            return 1;
        }
        const double seconds = std::chrono::duration<double>(t1 - t0).count();
        const double mbps = static_cast<double>(total) / (1024.0 * 1024.0) / seconds;
        std::printf("%s run %d: %.1f MB/s (%.3f s)\n", wl->name, run, mbps, seconds);
    }

    group.stop();
    group.join();
    return 0;
}
