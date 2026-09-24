#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
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

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpTransport.h>
#include <fiber/net/SocketAddress.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TlsServerHandshakeConfig.h>
#include <fiber/net/detail/TlsStreamFd.h>
#include <fiber/tls/TlsTicketService.h>

#include <openssl/ssl.h>

#include <poll.h>
#include "LoopTestSupport.h"

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

struct TestTlsPair {
    std::unique_ptr<fiber::net::TlsCredential> server_credential;
    fiber::net::TlsServerParam server_options;
    fiber::net::TlsClientParam client_options;
};

fiber::common::IoResult<TestTlsPair> create_tls_pair(const std::string &cert_path, const std::string &key_path) {
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(cert_path);
    credential_options.private_key = fiber::net::TlsPemSource::from_file(key_path);
    auto server_credential = fiber::net::TlsCredential::create(credential_options);
    if (!server_credential) {
        return std::unexpected(server_credential.error());
    }
    TestTlsPair pair{};
    pair.server_credential = std::move(*server_credential);
    pair.server_options.configure_callback = &fiber::net::configure_tls_with_credential;
    pair.server_options.configure_ctx = pair.server_credential.get();
    return pair;
}

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
        // The client stream may come back detached from another loop: adopt it
        // here before it is torn down on this loop.
        client_stream->adopt_loop(fiber::event::EventLoop::current());
        client_stream->close();
        delete client_stream;
    }
    done->set_value();
    co_return;
}

DetachedTask run_tls_server(fiber::net::detail::TlsStreamFd *server_stream, const fiber::net::TlsServerParam &param,
                            std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await server_stream->handshake(param);
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

DetachedTask run_tls_client(fiber::net::detail::TlsStreamFd *client_stream, const fiber::net::TlsClientParam &param,
                            std::promise<fiber::common::IoResult<std::string>> *done) {
    // The stream was constructed on another loop: take it over here before
    // the handshake re-registers the fd on this loop.
    client_stream->adopt_loop(fiber::event::EventLoop::current());
    auto handshake_result = co_await client_stream->handshake(param);
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

DetachedTask reset_tls_server_after_client_handshake(fiber::net::detail::TlsStreamFd *server_stream,
                                                     const fiber::net::TlsServerParam &param,
                                                     std::atomic_bool *client_handshake_done,
                                                     std::atomic_bool *server_closed,
                                                     std::promise<fiber::common::IoErr> *done) {
    auto handshake_result = co_await server_stream->handshake(param);
    if (!handshake_result) {
        server_closed->store(true, std::memory_order_release);
        done->set_value(handshake_result.error());
        co_return;
    }

    while (!client_handshake_done->load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(1ms);
    }

    linger reset_linger{1, 0};
    if (::setsockopt(server_stream->fd(), SOL_SOCKET, SO_LINGER, &reset_linger, sizeof(reset_linger)) != 0) {
        const fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
        server_stream->close();
        server_closed->store(true, std::memory_order_release);
        done->set_value(err);
        co_return;
    }
    server_stream->close();
    server_closed->store(true, std::memory_order_release);
    done->set_value(fiber::common::IoErr::None);
}

DetachedTask write_tls_after_server_reset(fiber::net::detail::TlsStreamFd *client_stream,
                                          const fiber::net::TlsClientParam &param,
                                          std::atomic_bool *client_handshake_done, std::atomic_bool *server_closed,
                                          std::promise<fiber::common::IoErr> *done) {
    // The stream was constructed on another loop: take it over here before
    // the handshake re-registers the fd on this loop.
    client_stream->adopt_loop(fiber::event::EventLoop::current());
    auto handshake_result = co_await client_stream->handshake(param);
    client_handshake_done->store(true, std::memory_order_release);
    if (!handshake_result) {
        done->set_value(handshake_result.error());
        co_return;
    }

    while (!server_closed->load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(1ms);
    }
    const char payload[] = "ping";
    auto write_result = co_await client_stream->write(payload, sizeof(payload) - 1U);
    done->set_value(write_result ? fiber::common::IoErr::None : write_result.error());
}

TEST(TlsStreamFdTest, CrossLoopHandshakeAndReadWriteUseOwnerPoller) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[0]);
        auto *client_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[1]);

        std::promise<fiber::common::IoResult<std::string>> server_promise;
        std::promise<fiber::common::IoResult<std::string>> client_promise;
        auto server_future = server_promise.get_future();
        auto client_future = client_promise.get_future();

        fiber::async::spawn(group.at(0),
                            [&]() { return run_tls_server(server_stream, tls_pair->server_options, &server_promise); });
        fiber::async::spawn(group.at(1),
                            [&]() { return run_tls_client(client_stream, tls_pair->client_options, &client_promise); });

        ASSERT_EQ(server_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(client_future.wait_for(2s), std::future_status::ready);

        auto server_result = server_future.get();
        auto client_result = client_future.get();
        ASSERT_TRUE(server_result);
        ASSERT_TRUE(client_result);
        EXPECT_EQ(*server_result, "ping");
        EXPECT_EQ(*client_result, "pong");

        // Return the client stream to loop 0 before both streams die there.
        std::promise<fiber::common::IoErr> handback_promise;
        auto handback_future = handback_promise.get_future();
        fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
            handback_promise.set_value(client_stream->detach_for_handover());
            co_return;
        });
        ASSERT_EQ(handback_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(handback_future.get(), fiber::common::IoErr::None);

        std::promise<void> close_promise;
        auto close_future = close_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return close_tls_streams(server_stream, client_stream, &close_promise); });
        ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);

        group.stop();
        group.join();
    });
}

TEST(TlsStreamFdTest, CrossLoopWriteFailureDoesNotTouchOwnerPoller) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_reset", kSelfSignedCertPem);
        TempFile key("key_reset", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[0]);
        auto *client_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[1]);

        std::atomic_bool client_handshake_done = false;
        std::atomic_bool server_closed = false;
        std::promise<fiber::common::IoErr> server_promise;
        std::promise<fiber::common::IoErr> client_promise;
        auto server_future = server_promise.get_future();
        auto client_future = client_promise.get_future();

        fiber::async::spawn(group.at(0), [&]() {
            return reset_tls_server_after_client_handshake(server_stream, tls_pair->server_options,
                                                           &client_handshake_done, &server_closed, &server_promise);
        });
        fiber::async::spawn(group.at(1), [&]() {
            return write_tls_after_server_reset(client_stream, tls_pair->client_options, &client_handshake_done,
                                                &server_closed, &client_promise);
        });

        ASSERT_EQ(server_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(client_future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(server_future.get(), fiber::common::IoErr::None);
        EXPECT_NE(client_future.get(), fiber::common::IoErr::None);

        // Return the client stream to loop 0 before both streams die there.
        std::promise<fiber::common::IoErr> handback_promise;
        auto handback_future = handback_promise.get_future();
        fiber::async::spawn(group.at(1), [&]() -> fiber::async::DetachedTask {
            handback_promise.set_value(client_stream->detach_for_handover());
            co_return;
        });
        ASSERT_EQ(handback_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(handback_future.get(), fiber::common::IoErr::None);

        std::promise<void> close_promise;
        auto close_future = close_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return close_tls_streams(server_stream, client_stream, &close_promise); });
        ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);

        group.stop();
        group.join();
    });
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

struct PollWriteStats {
    std::size_t written = 0;
    std::size_t would_block_count = 0;
};

struct AbandonPendingWriteStats {
    bool different_chain_busy_before = false;
    bool empty_chain_ready_after = false;
};

DetachedTask hold_tls_transport_after_handshake(fiber::http::TlsTransport *transport,
                                                const fiber::net::TlsServerParam &param,
                                                std::promise<fiber::common::IoResult<void>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    co_await fiber::async::sleep(200ms);
    done->set_value(fiber::common::IoResult<void>{});
    co_return;
}

DetachedTask abandon_blocked_tls_write(fiber::http::TlsTransport *transport, const fiber::net::TlsClientParam &param,
                                       fiber::mem::IoBufChain chain,
                                       std::promise<fiber::common::IoResult<AbandonPendingWriteStats>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    while (chain.readable_bytes() > 0) {
        std::size_t out = 0;
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = transport->poll_writev(chain, out, wait_event);
        if (err == fiber::common::IoErr::WouldBlock) {
            auto &empty_pool = ::fiber::event::EventLoop::current().io_buf_node_pool();
            fiber::mem::IoBufChain empty_chain;
            AbandonPendingWriteStats stats;
            stats.different_chain_busy_before =
                    transport->poll_writev(empty_chain, out, wait_event) == fiber::common::IoErr::Busy;
            transport->abandon_pending_io();
            stats.empty_chain_ready_after =
                    transport->poll_writev(empty_chain, out, wait_event) == fiber::common::IoErr::None;
            done->set_value(stats);
            co_return;
        }
        if (err != fiber::common::IoErr::None) {
            done->set_value(std::unexpected(err));
            co_return;
        }
        if (out == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
    }

    done->set_value(std::unexpected(fiber::common::IoErr::Unknown));
    co_return;
}

DetachedTask run_poll_transport_server(fiber::http::TlsTransport *transport, const fiber::net::TlsServerParam &param,
                                       std::size_t expected_size,
                                       std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    // Let the client fill its small send buffer so poll_writev must retain and
    // retry a coalesced TLS group after WouldBlock.
    co_await fiber::async::sleep(50ms);

    std::string received;
    received.reserve(expected_size);
    std::array<char, 16384> read_buf{};
    while (received.size() < expected_size) {
        std::size_t out = 0;
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = transport->poll_read(read_buf.data(), read_buf.size(), out, wait_event);
        if (err == fiber::common::IoErr::WouldBlock) {
            if (wait_event != fiber::event::IoEvent::Read && wait_event != fiber::event::IoEvent::Write) {
                done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
                co_return;
            }
            co_await fiber::async::sleep(1ms);
            continue;
        }
        if (err != fiber::common::IoErr::None) {
            done->set_value(std::unexpected(err));
            co_return;
        }
        if (out == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        received.append(read_buf.data(), out);
    }

    done->set_value(std::move(received));
    co_return;
}

DetachedTask run_poll_transport_client(fiber::http::TlsTransport *transport, const fiber::net::TlsClientParam &param,
                                       fiber::mem::IoBufChain chain,
                                       std::promise<fiber::common::IoResult<PollWriteStats>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    PollWriteStats stats;
    while (chain.readable_bytes() > 0) {
        std::size_t out = 0;
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = transport->poll_writev(chain, out, wait_event);
        if (err == fiber::common::IoErr::WouldBlock) {
            if (wait_event != fiber::event::IoEvent::Read && wait_event != fiber::event::IoEvent::Write) {
                done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
                co_return;
            }
            ++stats.would_block_count;
            co_await fiber::async::sleep(1ms);
            continue;
        }
        if (err != fiber::common::IoErr::None) {
            done->set_value(std::unexpected(err));
            co_return;
        }
        if (out == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        stats.written += out;
    }

    done->set_value(stats);
    co_return;
}

DetachedTask run_transport_server(fiber::http::TlsTransport *transport, const fiber::net::TlsServerParam &param,
                                  std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
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

DetachedTask run_transport_client(fiber::http::TlsTransport *transport, const fiber::net::TlsClientParam &param,
                                  fiber::mem::IoBufChain chain,
                                  std::promise<fiber::common::IoResult<std::size_t>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    std::size_t total_written = 0;
    while (chain.readable_bytes() != 0) {
        auto write_result = co_await transport->writev(chain, 5s);
        if (!write_result) {
            done->set_value(std::unexpected(write_result.error()));
            co_return;
        }
        if (*write_result == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        total_written += *write_result;
    }

    // Close-notify so the server sees EOF after the payload.
    (void) co_await transport->shutdown(5s);
    done->set_value(total_written);
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

DetachedTask read_tls_pending_payload(fiber::http::TlsTransport *transport, const fiber::net::TlsServerParam &param,
                                      std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
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

DetachedTask write_tls_pending_payload(fiber::http::TlsTransport *transport, const fiber::net::TlsClientParam &param,
                                       std::string payload, std::promise<fiber::common::IoResult<std::size_t>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    auto write_result = co_await transport->write(payload.data(), payload.size(), 5s);
    done->set_value(std::move(write_result));
    co_return;
}

TEST(TlsStreamFdTest, TlsTransportWaitReadableSeesPendingDecryptedData) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
        auto server_transport_result =
                fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer));
        auto client_transport_result =
                fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer));
        ASSERT_TRUE(server_transport_result);
        ASSERT_TRUE(client_transport_result);
        auto *server_transport = server_transport_result->release();
        auto *client_transport = client_transport_result->release();

        std::string payload(4096, 'p');
        std::promise<fiber::common::IoResult<std::string>> server_promise;
        std::promise<fiber::common::IoResult<std::size_t>> client_promise;
        auto server_future = server_promise.get_future();
        auto client_future = client_promise.get_future();

        fiber::async::spawn(group.at(0), [&]() {
            return read_tls_pending_payload(server_transport, tls_pair->server_options, &server_promise);
        });
        fiber::async::spawn(group.at(1), [&]() {
            return write_tls_pending_payload(client_transport, tls_pair->client_options, payload, &client_promise);
        });

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
    });
}

// Exercises TlsTransport::writev coalescing over a real TLS pair. The chain mixes
// small nodes (coalesced into <=8k groups), a >8k node (solo, zero-copy), and
// enough nodes to exceed the 16-iovec snapshot cap (forces a re-snapshot).
TEST(TlsStreamFdTest, TlsTransportWritevCoalescesMultiNodeChain) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
        auto server_transport_result =
                fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer));
        auto client_transport_result =
                fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer));
        ASSERT_TRUE(server_transport_result);
        ASSERT_TRUE(client_transport_result);
        auto *server_transport = server_transport_result->release();
        auto *client_transport = client_transport_result->release();


        fiber::mem::IoBufChain chain;
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

        fiber::async::spawn(group.at(0), [&]() {
            return run_transport_server(server_transport, tls_pair->server_options, &server_promise);
        });
        fiber::async::spawn(group.at(1), [&]() {
            return run_transport_client(client_transport, tls_pair->client_options, std::move(chain), &client_promise);
        });

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
    });
}

DetachedTask write_chain_recording_records(fiber::http::TlsTransport *transport,
                                           const fiber::net::TlsClientParam &param, fiber::mem::IoBufChain chain,
                                           std::promise<fiber::common::IoResult<std::vector<std::size_t>>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    // Each successful poll_writev is exactly one SSL_write, i.e. one record's
    // worth of plaintext.
    std::vector<std::size_t> records;
    while (chain.readable_bytes() != 0) {
        std::size_t out = 0;
        fiber::event::IoEvent wait_event = fiber::event::IoEvent::None;
        fiber::common::IoErr err = transport->poll_writev(chain, out, wait_event);
        if (err == fiber::common::IoErr::WouldBlock) {
            // The pending group is retained; the peer drains continuously.
            co_await fiber::async::sleep(1ms);
            continue;
        }
        if (err != fiber::common::IoErr::None) {
            done->set_value(std::unexpected(err));
            co_return;
        }
        if (out == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        records.push_back(out);
    }
    (void) co_await transport->shutdown(5s);
    done->set_value(std::move(records));
    co_return;
}

TEST(TlsStreamFdTest, TlsTransportWritevFillsRecordsAcrossSmallAndLargeNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
        auto server_transport_result =
                fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer));
        auto client_transport_result =
                fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer));
        ASSERT_TRUE(server_transport_result);
        ASSERT_TRUE(client_transport_result);
        auto *server_transport = server_transport_result->release();
        auto *client_transport = client_transport_result->release();


        fiber::mem::IoBufChain chain;
        // HTTP/2 DATA frame shape: 9-byte header node + 16 KiB payload node, three
        // times, then an oversized node followed by small nodes.
        std::vector<std::size_t> sizes = {9, 16384, 9, 16384, 9, 16384, 20000, 9, 300};
        std::string expected = build_distinct_chain(pool, chain, sizes);

        std::promise<fiber::common::IoResult<std::string>> server_promise;
        std::promise<fiber::common::IoResult<std::vector<std::size_t>>> client_promise;
        auto server_future = server_promise.get_future();
        auto client_future = client_promise.get_future();

        fiber::async::spawn(group.at(0), [&]() {
            return run_transport_server(server_transport, tls_pair->server_options, &server_promise);
        });
        fiber::async::spawn(group.at(1), [&]() {
            return write_chain_recording_records(client_transport, tls_pair->client_options, std::move(chain),
                                                 &client_promise);
        });

        ASSERT_EQ(client_future.wait_for(10s), std::future_status::ready);
        ASSERT_EQ(server_future.wait_for(10s), std::future_status::ready);

        auto client_result = client_future.get();
        auto server_result = server_future.get();
        ASSERT_TRUE(client_result);
        ASSERT_TRUE(server_result);
        EXPECT_EQ(*server_result, expected);
        // 3 x (9 + 16384) = 49179 -> three full records + 27 bytes carried into
        // the fourth, which is filled from the 20000 node; its 3643-byte tail is
        // coalesced with the trailing [9][300] instead of becoming its own record.
        const std::vector<std::size_t> expected_records = {16384, 16384, 16384, 16384, 3643 + 9 + 300};
        EXPECT_EQ(*client_result, expected_records);

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
    });
}

TEST(TlsStreamFdTest, TlsTransportPollWritevRetainsCoalescedGroupAcrossWouldBlock) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
        int send_buffer_size = 4096;
        ASSERT_EQ(::setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
        auto server_transport_result =
                fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer));
        auto client_transport_result =
                fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer));
        ASSERT_TRUE(server_transport_result);
        ASSERT_TRUE(client_transport_result);
        auto *server_transport = server_transport_result->release();
        auto *client_transport = client_transport_result->release();


        fiber::mem::IoBufChain chain;
        std::vector<std::size_t> sizes(64, 4096);
        std::string expected = build_distinct_chain(pool, chain, sizes);

        std::promise<fiber::common::IoResult<std::string>> server_promise;
        std::promise<fiber::common::IoResult<PollWriteStats>> client_promise;
        auto server_future = server_promise.get_future();
        auto client_future = client_promise.get_future();

        fiber::async::spawn(group.at(0), [&]() {
            return run_poll_transport_server(server_transport, tls_pair->server_options, expected.size(),
                                             &server_promise);
        });
        fiber::async::spawn(group.at(1), [&]() {
            return run_poll_transport_client(client_transport, tls_pair->client_options, std::move(chain),
                                             &client_promise);
        });

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
        EXPECT_EQ(client_result->written, expected.size());
        EXPECT_GT(client_result->would_block_count, 0U);
        EXPECT_EQ(*server_result, expected);
    });
}

TEST(TlsStreamFdTest, TlsTransportAbandonPendingWriteDropsChainReference) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
        int send_buffer_size = 4096;
        ASSERT_EQ(::setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &send_buffer_size, sizeof(send_buffer_size)), 0);

        fiber::event::EventLoopGroup group(2);
        group.start();

        fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
        auto server_transport_result =
                fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(fds[0], peer));
        auto client_transport_result =
                fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(fds[1], peer));
        ASSERT_TRUE(server_transport_result);
        ASSERT_TRUE(client_transport_result);
        auto *server_transport = server_transport_result->release();
        auto *client_transport = client_transport_result->release();


        fiber::mem::IoBufChain chain;
        std::vector<std::size_t> sizes(64, 4096);
        (void) build_distinct_chain(pool, chain, sizes);

        std::promise<fiber::common::IoResult<void>> server_promise;
        std::promise<fiber::common::IoResult<AbandonPendingWriteStats>> client_promise;
        auto server_future = server_promise.get_future();
        auto client_future = client_promise.get_future();

        fiber::async::spawn(group.at(0), [&]() {
            return hold_tls_transport_after_handshake(server_transport, tls_pair->server_options, &server_promise);
        });
        fiber::async::spawn(group.at(1), [&]() {
            return abandon_blocked_tls_write(client_transport, tls_pair->client_options, std::move(chain),
                                             &client_promise);
        });

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

        ASSERT_TRUE(server_result);
        ASSERT_TRUE(client_result);
        EXPECT_TRUE(client_result->different_chain_busy_before);
        EXPECT_TRUE(client_result->empty_chain_ready_after);
    });
}

// ---- stateless ticket assembly (09 slice 3): a BoringSSL socket oracle ----
//
// The tests below drive OUR TlsStreamFd server from a real BoringSSL client
// over the socketpair (blocking I/O on the test thread; the server runs on an
// EventLoopGroup thread). BoringSSL's session stash is the observable: the
// new-session callback fires exactly when a NewSessionTicket (1.3) or
// session-ticket handshake message (1.2) arrives, so "no service configured
// → no ticket" and "same material after a rebuild → resumption" are both
// assertable against an independent implementation.

std::vector<SSL_SESSION *> &stashed_sessions() {
    static std::vector<SSL_SESSION *> stash;
    return stash;
}

int stash_new_session(SSL *, SSL_SESSION *sess) noexcept {
    stashed_sessions().push_back(sess);
    return 1; // the stash took ownership
}

struct StashedSessionsGuard {
    ~StashedSessionsGuard() {
        for (SSL_SESSION *sess: stashed_sessions()) {
            SSL_SESSION_free(sess);
        }
        stashed_sessions().clear();
    }
};

struct BsslSocketClient {
    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;

    // tls12_only pins the version; collect_tickets routes received tickets
    // into the stash above.
    static bool make(bool tls12_only, bool collect_tickets, std::unique_ptr<BsslSocketClient> &out) {
        auto client = std::unique_ptr<BsslSocketClient>(new BsslSocketClient);
        client->ctx = SSL_CTX_new(TLS_client_method());
        if (client->ctx == nullptr) {
            return false;
        }
        SSL_CTX_set_verify(client->ctx, SSL_VERIFY_NONE, nullptr); // self-signed test credential
        if (tls12_only) {
            SSL_CTX_set_min_proto_version(client->ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(client->ctx, TLS1_2_VERSION);
        } else {
            SSL_CTX_set_min_proto_version(client->ctx, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(client->ctx, TLS1_3_VERSION);
        }
        if (collect_tickets) {
            SSL_CTX_set_session_cache_mode(client->ctx, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
            SSL_CTX_sess_set_new_cb(client->ctx, &stash_new_session);
        }
        client->ssl = SSL_new(client->ctx);
        if (client->ssl == nullptr) {
            return false;
        }
        SSL_set_connect_state(client->ssl);
        out = std::move(client);
        return true;
    }

    ~BsslSocketClient() {
        if (ssl != nullptr) {
            SSL_free(ssl);
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }

    BsslSocketClient() = default;
    BsslSocketClient(const BsslSocketClient &) = delete;
    BsslSocketClient &operator=(const BsslSocketClient &) = delete;

    void attach_fd(int fd) { SSL_set_bio(ssl, BIO_new_socket(fd, BIO_NOCLOSE), BIO_new_socket(fd, BIO_NOCLOSE)); }

    bool handshake() { return SSL_do_handshake(ssl) == 1; }

    bool round_trip(const char *request, const char *expect_reply) {
        const int request_len = static_cast<int>(std::strlen(request));
        if (SSL_write(ssl, request, request_len) != request_len) {
            return false;
        }
        char buffer[64] = {};
        const int got = SSL_read(ssl, buffer, sizeof(buffer) - 1);
        if (got != static_cast<int>(std::strlen(expect_reply))) {
            return false;
        }
        return std::memcmp(buffer, expect_reply, static_cast<std::size_t>(got)) == 0;
    }

    // The 1.3 NST trails the handshake flight; process pending records (the
    // read path digests handshake messages and fires the stash callback)
    // until the stash holds a session or the wire stays quiet past the
    // deadline. Returns true when at least one session was stashed.
    bool slurp_tickets(int fd, int attempts, int per_attempt_ms) {
        for (int i = 0; i < attempts && stashed_sessions().empty(); ++i) {
            pollfd watched{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&watched, 1, per_attempt_ms) == 1 && (watched.revents & POLLIN) != 0) {
                char scratch[512];
                (void) SSL_read(ssl, scratch, sizeof(scratch));
            }
        }
        return !stashed_sessions().empty();
    }
};

// Bounds every blocking client read so a wedged server fails the test
// instead of hanging it.
void arm_receive_timeout(int fd) {
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    (void) ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// One server stream against one external blocking client: handshake, then a
// ping/pong exchange, then teardown. Returns the server's observed request.
// await_ticket keeps reading (bounded) after the round trip so a trailing
// 1.3 NST reaches the stash before the fd closes.
std::string run_server_against_bssl_client(fiber::event::EventLoopGroup &group, int server_fd,
                                           fiber::net::TlsServerParam &param, BsslSocketClient &client, int client_fd,
                                           std::string_view expect_request, bool await_ticket) {
    auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), server_fd);
    std::promise<fiber::common::IoResult<std::string>> server_promise;
    auto server_future = server_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return run_tls_server(server_stream, param, &server_promise); });

    if (!client.handshake()) {
        return "client-handshake-failed";
    }
    if (!client.round_trip(expect_request.data(), "pong")) {
        return "client-round-trip-failed";
    }
    if (await_ticket && !client.slurp_tickets(client_fd, 20, 100)) {
        return "no-ticket-received";
    }

    std::string observed;
    if (server_future.wait_for(5s) == std::future_status::ready) {
        auto result = server_future.get();
        observed = result ? *result : "server-error";
    } else {
        observed = "server-timeout";
    }

    // Our close_notify only (no wait for the peer's — nobody drives the
    // server read side anymore), then the fd; the server stream is torn down
    // on its own loop.
    (void) SSL_shutdown(client.ssl);
    ::close(client_fd);
    std::promise<void> close_done;
    auto close_future = close_done.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_tls_streams(server_stream, nullptr, &close_done); });
    (void) close_future.wait_for(2s);
    return observed;
}

// Decision 1 (09 §1): no ticket service configured → the server never sends
// a NewSessionTicket. BoringSSL's stash staying empty is the proof; the
// handshake and app data still flow.
TEST(TlsStreamFdTest, UnconfiguredTicketServiceMintsNoSessionTicket) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        StashedSessionsGuard stash_guard;
        TempFile cert("cert", kSelfSignedCertPem);
        TempFile key("key", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);
        EXPECT_EQ(tls_pair->server_options.ticket_service, nullptr); // the default under test

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        arm_receive_timeout(fds[1]);

        fiber::event::EventLoopGroup group(1);
        group.start();

        std::unique_ptr<BsslSocketClient> client;
        ASSERT_TRUE(BsslSocketClient::make(false, true, client));
        client->attach_fd(fds[1]);

        const std::string observed =
                run_server_against_bssl_client(group, fds[0], tls_pair->server_options, *client, fds[1], "ping", false);
        EXPECT_EQ(observed, "ping");
        EXPECT_TRUE(stashed_sessions().empty());

        group.stop();
        group.join();
    });
}

// The stateless contract end to end: a ticket minted under one service stays
// openable by a FRESH service built from the same material (a restart), and
// BoringSSL resumes through it. Both protocol versions.
TEST(TlsStreamFdTest, BoringsslClientResumesAcrossTicketServiceRebuild) {
    SigpipeGuard sigpipe_guard;
    StashedSessionsGuard stash_guard;
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);

    auto tls_pair = create_tls_pair(cert.path, key.path);
    ASSERT_TRUE(tls_pair);

    fiber::event::EventLoopGroup group(1);
    group.start();

    for (const bool tls12_only: {false, true}) {
        SCOPED_TRACE(tls12_only ? "tls12" : "tls13");
        stashed_sessions().clear();

        // Fixed material, born a minute ago (inside the mint window). The
        // phase-2 service is built from the same bytes — that is the restart.
        std::array<fiber::tls::TlsTicketKeyMaterial, 1> material{};
        material[0].id = 7;
        material[0].created_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count() -
                                 60'000;
        for (std::size_t i = 0; i < material[0].bytes.size(); ++i) {
            material[0].bytes[i] = static_cast<std::uint8_t>(i);
        }
        const fiber::tls::TlsTicketKeyPolicy policy{};

        // Phase 1: full handshake against service A; the client banks the
        // ticket.
        fiber::tls::TlsTicketService service_a({material}, policy);
        ASSERT_TRUE(service_a.valid());
        fiber::net::TlsServerParam param = tls_pair->server_options;
        param.ticket_service = &service_a;

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        arm_receive_timeout(fds[1]);

        std::unique_ptr<BsslSocketClient> collector;
        ASSERT_TRUE(BsslSocketClient::make(tls12_only, true, collector));
        collector->attach_fd(fds[1]);
        EXPECT_EQ(run_server_against_bssl_client(group, fds[0], param, *collector, fds[1], "ping", true), "ping");
        // The 1.2 ticket rides the flight (consumed by the handshake); the
        // 1.3 NST may trail it — the helper slurped either way.
        ASSERT_EQ(stashed_sessions().size(), 1u) << "exactly one ticket minted";
        SSL_SESSION *banked = stashed_sessions().back();

        // Phase 2: a brand-new service from the same material, a brand-new
        // stream — the client presents the banked ticket and must resume.
        fiber::tls::TlsTicketService service_b({material}, policy);
        ASSERT_TRUE(service_b.valid());
        param.ticket_service = &service_b;

        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        arm_receive_timeout(fds[1]);

        std::unique_ptr<BsslSocketClient> resumer;
        ASSERT_TRUE(BsslSocketClient::make(tls12_only, false, resumer));
        resumer->attach_fd(fds[1]);
        ASSERT_EQ(SSL_set_session(resumer->ssl, banked), 1);
        EXPECT_EQ(run_server_against_bssl_client(group, fds[0], param, *resumer, fds[1], "ping", false), "ping");
        EXPECT_EQ(SSL_session_reused(resumer->ssl), 1) << "the rebuilt service resumed the banked ticket";
    }

    group.stop();
    group.join();
}

} // namespace
