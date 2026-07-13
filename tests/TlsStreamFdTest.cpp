#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <memory>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "async/Spawn.h"
#include "async/Task.h"
#include "common/IoError.h"
#include "common/mem/IoBuf.h"
#include "common/mem/IoBufChain.h"
#include "event/EventLoopGroup.h"
#include "http/HttpTransport.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"
#include "net/TlsContext.h"
#include "net/TlsOptions.h"
#include "net/detail/TlsStreamFd.h"

namespace {

using fiber::async::DetachedTask;
using namespace std::chrono_literals;

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
    std::string path = "/tmp/fiber_tls_stream_fd_";
    path.append(tag);
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::getpid())));
    path.push_back('_');
    path.append(std::to_string(static_cast<long>(::random())));
    path.append(".pem");
    return path;
}

bool write_file(const std::string &path, std::string_view data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

struct TempFile {
    std::string path;
    bool ok = false;

    TempFile(const char *tag, std::string_view data) {
        path = make_temp_path(tag);
        ok = write_file(path, data);
        if (!ok) {
            path.clear();
        }
    }

    ~TempFile() {
        if (!path.empty()) {
            ::unlink(path.c_str());
        }
    }
};

struct SigpipeGuard {
    using Handler = void (*)(int);

    Handler old = SIG_DFL;

    SigpipeGuard() { old = ::signal(SIGPIPE, SIG_IGN); }

    ~SigpipeGuard() { (void) ::signal(SIGPIPE, old); }
};

DetachedTask close_tls_streams(fiber::net::detail::TlsStreamFd *server_stream,
                               fiber::net::detail::TlsStreamFd *client_stream, std::promise<void> *done) {
    if (server_stream) {
        server_stream->close();
        delete server_stream;
    }
    if (client_stream) {
        client_stream->close();
        delete client_stream;
    }
    done->set_value();
    co_return;
}

DetachedTask run_tls_server(fiber::net::detail::TlsStreamFd *server_stream,
                            std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await server_stream->handshake();
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    std::array<char, 32> read_buf{};
    auto read_result = co_await server_stream->read(read_buf.data(), read_buf.size());
    if (!read_result) {
        done->set_value(std::unexpected(read_result.error()));
        co_return;
    }

    const char reply[] = "pong";
    auto write_result = co_await server_stream->write(reply, sizeof(reply) - 1U);
    if (!write_result) {
        done->set_value(std::unexpected(write_result.error()));
        co_return;
    }

    done->set_value(std::string(read_buf.data(), *read_result));
    co_return;
}

DetachedTask run_tls_client(fiber::net::detail::TlsStreamFd *client_stream,
                            std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await client_stream->handshake();
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    const char request[] = "ping";
    auto write_result = co_await client_stream->write(request, sizeof(request) - 1U);
    if (!write_result) {
        done->set_value(std::unexpected(write_result.error()));
        co_return;
    }

    std::array<char, 32> read_buf{};
    auto read_result = co_await client_stream->read(read_buf.data(), read_buf.size());
    if (!read_result) {
        done->set_value(std::unexpected(read_result.error()));
        co_return;
    }

    done->set_value(std::string(read_buf.data(), *read_result));
    co_return;
}

TEST(TlsStreamFdTest, CrossLoopHandshakeAndReadWriteUseOwnerPoller) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::net::TlsOptions server_options{};
    server_options.cert_file = cert.path;
    server_options.key_file = key.path;
    fiber::net::TlsContext server_ctx(std::move(server_options), true);
    ASSERT_TRUE(server_ctx.init());

    fiber::net::TlsOptions client_options{};
    fiber::net::TlsContext client_ctx(std::move(client_options), false);
    ASSERT_TRUE(client_ctx.init());

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(2);
    group.start();

    auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[0]);
    auto *client_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[1]);
    ASSERT_TRUE(server_stream->init(server_ctx.raw(), true));
    ASSERT_TRUE(client_stream->init(client_ctx.raw(), false));

    std::promise<fiber::common::IoResult<std::string>> server_promise;
    std::promise<fiber::common::IoResult<std::string>> client_promise;
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return run_tls_server(server_stream, &server_promise); });
    fiber::async::spawn(group.at(1), [&]() { return run_tls_client(client_stream, &client_promise); });

    ASSERT_EQ(server_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(client_future.wait_for(2s), std::future_status::ready);

    auto server_result = server_future.get();
    auto client_result = client_future.get();
    ASSERT_TRUE(server_result);
    ASSERT_TRUE(client_result);
    EXPECT_EQ(*server_result, "ping");
    EXPECT_EQ(*client_result, "pong");

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_tls_streams(server_stream, client_stream, &close_promise); });
    ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);

    group.stop();
    group.join();
}

// Build an IoBufChain of segments with the given sizes. Each segment i is filled
// with a distinct byte (0x40 + i) so that reordering, drops, or duplication in the
// coalesce path show up as a mismatched byte. Returns the expected concatenation.
std::string build_distinct_chain(fiber::mem::IoBufNodePool &pool, fiber::mem::IoBufChain &chain,
                                 const std::vector<std::size_t> &sizes) {
    std::string expected;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        std::size_t n = sizes[i];
        fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(n);
        std::memset(buf.writable_data(), static_cast<int>(0x40 + (i & 0x3f)), n);
        buf.commit(n);
        chain.append(std::move(buf));
        expected.append(static_cast<std::size_t>(n), static_cast<char>(0x40 + (i & 0x3f)));
    }
    return expected;
}

DetachedTask run_transport_server(fiber::http::TlsTransport *transport,
                                  std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await transport->handshake(5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    std::string received;
    std::array<char, 8192> read_buf{};
    for (;;) {
        auto ready_result = co_await transport->wait_readable(5s);
        if (!ready_result) {
            done->set_value(std::unexpected(ready_result.error()));
            co_return;
        }

        auto read_result = co_await transport->read(read_buf.data(), read_buf.size(), 5s);
        if (!read_result) {
            done->set_value(std::unexpected(read_result.error()));
            co_return;
        }
        if (*read_result == 0) {
            break;
        }
        received.append(read_buf.data(), *read_result);
    }
    done->set_value(std::move(received));
    co_return;
}

DetachedTask run_transport_client(fiber::http::TlsTransport *transport, fiber::mem::IoBufChain chain,
                                  std::promise<fiber::common::IoResult<std::size_t>> *done) {
    auto handshake_result = co_await transport->handshake(5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    auto write_result = co_await transport->writev(chain, 5s);
    if (!write_result) {
        done->set_value(std::unexpected(write_result.error()));
        co_return;
    }

    // Close-notify so the server sees EOF after the payload.
    (void) co_await transport->shutdown(5s);
    done->set_value(*write_result);
    co_return;
}

DetachedTask close_transport(fiber::http::TlsTransport *transport, std::promise<void> *done) {
    if (transport) {
        transport->close();
        delete transport;
    }
    done->set_value();
    co_return;
}

DetachedTask read_tls_pending_payload(fiber::http::TlsTransport *transport,
                                      std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await transport->handshake(5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    auto ready_result = co_await transport->wait_readable(5s);
    if (!ready_result) {
        done->set_value(std::unexpected(ready_result.error()));
        co_return;
    }

    std::array<char, 1024> first{};
    auto first_result = co_await transport->read(first.data(), first.size(), 5s);
    if (!first_result) {
        done->set_value(std::unexpected(first_result.error()));
        co_return;
    }
    if (*first_result != first.size()) {
        done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
        co_return;
    }

    // The peer sends exactly one application-data record. The first short read
    // leaves decrypted bytes inside BoringSSL while the socket itself has no new
    // data. A zero-timeout wait can only succeed through SSL_has_pending().
    auto pending_result = co_await transport->wait_readable(0ms);
    if (!pending_result) {
        done->set_value(std::unexpected(pending_result.error()));
        co_return;
    }

    std::array<char, 4096> rest{};
    auto rest_result = co_await transport->read(rest.data(), rest.size(), 5s);
    if (!rest_result) {
        done->set_value(std::unexpected(rest_result.error()));
        co_return;
    }

    std::string received(first.data(), *first_result);
    received.append(rest.data(), *rest_result);
    done->set_value(std::move(received));
    co_return;
}

DetachedTask write_tls_pending_payload(fiber::http::TlsTransport *transport, std::string payload,
                                       std::promise<fiber::common::IoResult<std::size_t>> *done) {
    auto handshake_result = co_await transport->handshake(5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    auto write_result = co_await transport->write(payload.data(), payload.size(), 5s);
    done->set_value(std::move(write_result));
    co_return;
}

TEST(TlsStreamFdTest, TlsTransportWaitReadableSeesPendingDecryptedData) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::net::TlsOptions server_options{};
    server_options.cert_file = cert.path;
    server_options.key_file = key.path;
    fiber::net::TlsContext server_ctx(std::move(server_options), true);
    ASSERT_TRUE(server_ctx.init());

    fiber::net::TlsOptions client_options{};
    fiber::net::TlsContext client_ctx(std::move(client_options), false);
    ASSERT_TRUE(client_ctx.init());

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto server_transport_result =
            fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer), server_ctx);
    auto client_transport_result =
            fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer), client_ctx);
    ASSERT_TRUE(server_transport_result);
    ASSERT_TRUE(client_transport_result);
    auto *server_transport = server_transport_result->release();
    auto *client_transport = client_transport_result->release();

    std::string payload(4096, 'p');
    std::promise<fiber::common::IoResult<std::string>> server_promise;
    std::promise<fiber::common::IoResult<std::size_t>> client_promise;
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return read_tls_pending_payload(server_transport, &server_promise); });
    fiber::async::spawn(group.at(1),
                        [&]() { return write_tls_pending_payload(client_transport, payload, &client_promise); });

    ASSERT_EQ(client_future.wait_for(10s), std::future_status::ready);
    ASSERT_EQ(server_future.wait_for(10s), std::future_status::ready);
    auto client_result = client_future.get();
    auto server_result = server_future.get();

    std::promise<void> server_close_promise;
    std::promise<void> client_close_promise;
    auto server_close_future = server_close_promise.get_future();
    auto client_close_future = client_close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_transport(server_transport, &server_close_promise); });
    fiber::async::spawn(group.at(1), [&]() { return close_transport(client_transport, &client_close_promise); });
    ASSERT_EQ(server_close_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(client_close_future.wait_for(2s), std::future_status::ready);

    group.stop();
    group.join();

    ASSERT_TRUE(client_result);
    ASSERT_TRUE(server_result);
    EXPECT_EQ(*client_result, payload.size());
    EXPECT_EQ(*server_result, payload);
}

// Exercises TlsTransport::writev coalescing over a real TLS pair. The chain mixes
// small nodes (coalesced into <=8k groups), a >8k node (solo, zero-copy), and
// enough nodes to exceed the 16-iovec snapshot cap (forces a re-snapshot).
TEST(TlsStreamFdTest, TlsTransportWritevCoalescesMultiNodeChain) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    fiber::net::TlsOptions server_options{};
    server_options.cert_file = cert.path;
    server_options.key_file = key.path;
    fiber::net::TlsContext server_ctx(std::move(server_options), true);
    ASSERT_TRUE(server_ctx.init());

    fiber::net::TlsOptions client_options{};
    fiber::net::TlsContext client_ctx(std::move(client_options), false);
    ASSERT_TRUE(client_ctx.init());

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

    fiber::event::EventLoopGroup group(2);
    group.start();

    fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
    auto server_transport_result =
            fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer), server_ctx);
    auto client_transport_result =
            fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer), client_ctx);
    ASSERT_TRUE(server_transport_result);
    ASSERT_TRUE(client_transport_result);
    auto *server_transport = server_transport_result->release();
    auto *client_transport = client_transport_result->release();

    fiber::mem::IoBufNodePool pool;
    fiber::mem::IoBufChain chain(pool);
    // [1k][2k][3k][7k][4k][2k] + oversized [20k] + 20x[100B] (27 nodes, >16 iov cap).
    std::vector<std::size_t> sizes = {1024, 2048, 3072, 7168, 4096, 2048, 20480};
    for (int i = 0; i < 20; ++i) {
        sizes.push_back(100);
    }
    std::string expected = build_distinct_chain(pool, chain, sizes);

    std::promise<fiber::common::IoResult<std::string>> server_promise;
    std::promise<fiber::common::IoResult<std::size_t>> client_promise;
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();

    fiber::async::spawn(group.at(0), [&]() { return run_transport_server(server_transport, &server_promise); });
    fiber::async::spawn(group.at(1),
                        [&]() { return run_transport_client(client_transport, std::move(chain), &client_promise); });

    ASSERT_EQ(client_future.wait_for(10s), std::future_status::ready);
    ASSERT_EQ(server_future.wait_for(10s), std::future_status::ready);

    auto client_result = client_future.get();
    auto server_result = server_future.get();
    ASSERT_TRUE(client_result);
    ASSERT_TRUE(server_result);
    EXPECT_EQ(*client_result, expected.size());
    EXPECT_EQ(*server_result, expected);

    std::promise<void> close_promise;
    auto close_future = close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_transport(server_transport, &close_promise); });
    ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);
    std::promise<void> close_promise2;
    auto close_future2 = close_promise2.get_future();
    fiber::async::spawn(group.at(1), [&]() { return close_transport(client_transport, &close_promise2); });
    ASSERT_EQ(close_future2.wait_for(2s), std::future_status::ready);

    group.stop();
    group.join();
}

} // namespace
