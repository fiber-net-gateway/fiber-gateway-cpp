#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/async/Timeout.h>
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

#include <fcntl.h>
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

// server_options points at server_credential, so the pair is built in place
// and never moved.
struct TestTlsPair {
    explicit TestTlsPair(fiber::net::TlsCredential credential) : server_credential(std::move(credential)) {
        server_options.configure_callback = &fiber::net::configure_tls_with_credential;
        server_options.configure_ctx = &server_credential;
    }
    TestTlsPair(TestTlsPair &&) = delete;

    fiber::net::TlsCredential server_credential;
    fiber::net::TlsServerParam server_options{};
    fiber::net::TlsClientParam client_options{};
};

fiber::common::IoResult<TestTlsPair> create_tls_pair(const std::string &cert_path, const std::string &key_path) {
    fiber::net::TlsCredentialOptions credential_options{};
    credential_options.certificate_chain = fiber::net::TlsPemSource::from_file(cert_path);
    credential_options.private_key = fiber::net::TlsPemSource::from_file(key_path);
    auto server_credential = fiber::net::TlsCredential::create(credential_options);
    if (!server_credential) {
        return std::unexpected(server_credential.error());
    }
    return fiber::common::IoResult<TestTlsPair>(std::in_place, std::move(*server_credential));
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

// The poll + wait loop the transport layer runs; these helpers keep the
// fd-layer smoke tests on the same production surface.
fiber::async::Task<fiber::common::IoResult<std::size_t>> tls_poll_read(fiber::net::detail::TlsStreamFd &stream,
                                                                       void *buf, std::size_t len) {
    fiber::mem::IoBufChain chain;
    auto read_result = co_await stream.readv(len, chain);
    if (!read_result) {
        co_return std::unexpected(read_result.error());
    }
    std::size_t copied = 0;
    for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr && copied < len; node = node->next) {
        const std::size_t take = std::min(node->buf.readable(), len - copied);
        std::memcpy(static_cast<std::uint8_t *>(buf) + copied, node->buf.readable_data(), take);
        copied += take;
    }
    co_return copied;
}

fiber::async::Task<fiber::common::IoResult<std::size_t>> tls_poll_write(fiber::net::detail::TlsStreamFd &stream,
                                                                        const void *buf, std::size_t len) {
    fiber::mem::IoBufChain chain;
    fiber::mem::IoBuf node = fiber::mem::IoBuf::allocate(len);
    if (!node.valid()) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    std::memcpy(node.writable_data(), buf, len);
    node.commit(len);
    if (!chain.append(std::move(node))) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    std::size_t total = 0;
    while (chain.readable_bytes() > 0) {
        auto write_result = co_await stream.writev(chain);
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }
        total += *write_result;
    }
    co_return total;
}

DetachedTask run_tls_server(fiber::net::detail::TlsStreamFd *server_stream, const fiber::net::TlsServerParam &param,
                            std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await server_stream->handshake(param);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    std::array<char, 32> read_buf{};
    auto read_result = co_await tls_poll_read(*server_stream, read_buf.data(), read_buf.size());
    if (!read_result) {
        done->set_value(std::unexpected(read_result.error()));
        co_return;
    }

    const char reply[] = "pong";
    auto write_result = co_await tls_poll_write(*server_stream, reply, sizeof(reply) - 1U);
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
    auto write_result = co_await tls_poll_write(*client_stream, request, sizeof(request) - 1U);
    if (!write_result) {
        done->set_value(std::unexpected(write_result.error()));
        co_return;
    }

    std::array<char, 32> read_buf{};
    auto read_result = co_await tls_poll_read(*client_stream, read_buf.data(), read_buf.size());
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
    auto write_result = co_await tls_poll_write(*client_stream, payload, sizeof(payload) - 1U);
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

// Regression for the frame-local staging redesign: a close() that interrupts a
// suspended handshake must unwind the handshake coroutine — its staging and
// engines die with the coroutine frame — and report Canceled, with nothing
// left for a later close()/destructor to clean up.
template<typename Param>
DetachedTask await_handshake_result(fiber::net::detail::TlsStreamFd *stream, const Param &param,
                                    std::promise<fiber::common::IoErr> *done) {
    auto handshake_result = co_await stream->handshake(param);
    done->set_value(handshake_result ? fiber::common::IoErr::None : handshake_result.error());
}

DetachedTask close_parked_handshake(fiber::net::detail::TlsStreamFd *stream, std::promise<void> *done) {
    // Let the parked-handshake task run into its socket wait first: it was
    // spawned before this task, so its whole first slice (start + park) runs
    // before this timer can fire — the margin only has to survive loop-thread
    // starvation, not task ordering.
    co_await fiber::async::sleep(50ms);
    // close() resumes the suspended handshake inline (Canceled wake): by the
    // time it returns, the coroutine has unwound and its frame is gone, so
    // the stream can be deleted here.
    stream->close();
    delete stream;
    done->set_value();
    co_return;
}

TEST(TlsStreamFdTest, CloseDuringSuspendedHandshakeUnwindsCleanly) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_close_susp", kSelfSignedCertPem);
        TempFile key("key_close_susp", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        int server_fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, server_fds), 0);
        int client_fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, client_fds), 0);

        fiber::event::EventLoopGroup group(1);
        group.start();

        // Server side: no ClientHello ever arrives (the peer stays silent), so
        // the handshake parks in wait_readable with live engines.
        auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), server_fds[0]);
        std::promise<fiber::common::IoErr> server_handshake_promise;
        auto server_handshake_future = server_handshake_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() {
            return await_handshake_result(server_stream, tls_pair->server_options, &server_handshake_promise);
        });
        std::promise<void> server_close_promise;
        auto server_close_future = server_close_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return close_parked_handshake(server_stream, &server_close_promise); });

        // Client side: the ClientHello is flushed, then the handshake parks
        // waiting for a ServerHello that never comes.
        auto *client_stream = new fiber::net::detail::TlsStreamFd(group.at(0), client_fds[0]);
        std::promise<fiber::common::IoErr> client_handshake_promise;
        auto client_handshake_future = client_handshake_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() {
            return await_handshake_result(client_stream, tls_pair->client_options, &client_handshake_promise);
        });
        std::promise<void> client_close_promise;
        auto client_close_future = client_close_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return close_parked_handshake(client_stream, &client_close_promise); });

        ASSERT_EQ(server_close_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(client_close_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(server_handshake_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(client_handshake_future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(server_handshake_future.get(), fiber::common::IoErr::Canceled);
        EXPECT_EQ(client_handshake_future.get(), fiber::common::IoErr::Canceled);

        ::close(server_fds[1]); // the silent peers
        ::close(client_fds[1]);

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
    std::vector<std::size_t> calls; // what each successful try_writev reported
};

DetachedTask run_poll_transport_server(fiber::http::TlsTransport *transport, const fiber::net::TlsServerParam &param,
                                       std::size_t expected_size,
                                       std::promise<fiber::common::IoResult<std::string>> *done) {
    auto handshake_result = co_await transport->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(std::unexpected(handshake_result.error()));
        co_return;
    }

    // Let the client fill its small send buffer: its try_writev then backs
    // off with WouldBlock while the sealed batch drains.
    co_await fiber::async::sleep(50ms);

    std::string received;
    received.reserve(expected_size);
    while (received.size() < expected_size) {
        fiber::mem::IoBufChain chunk;
        auto read_result = transport->try_readv(16384, chunk);
        if (!read_result) {
            if (read_result.error() == fiber::common::IoErr::WouldBlock) {
                co_await fiber::async::sleep(1ms);
                continue;
            }
            done->set_value(std::unexpected(read_result.error()));
            co_return;
        }
        if (*read_result == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        for (const fiber::mem::IoBufNode *node = chunk.front_node(); node != nullptr; node = node->next) {
            received.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
        }
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
        auto result = transport->try_writev(chain);
        if (!result) {
            if (result.error() == fiber::common::IoErr::WouldBlock) {
                ++stats.would_block_count;
                co_await fiber::async::sleep(1ms);
                continue;
            }
            done->set_value(std::unexpected(result.error()));
            co_return;
        }
        if (*result == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        stats.written += *result;
        stats.calls.push_back(*result);
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
    for (;;) {
        auto ready_result = co_await transport->wait_readable(5s);
        if (!ready_result) {
            done->set_value(std::unexpected(ready_result.error()));
            co_return;
        }

        fiber::mem::IoBufChain chunk;
        auto read_result = co_await transport->readv(8192, chunk, 5s);
        if (!read_result) {
            done->set_value(std::unexpected(read_result.error()));
            co_return;
        }
        if (*read_result == 0) {
            break;
        }
        for (const fiber::mem::IoBufNode *node = chunk.front_node(); node != nullptr; node = node->next) {
            received.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
        }
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
    std::size_t first_len = 0;
    {
        fiber::mem::IoBufChain first_chunk;
        auto first_result = co_await transport->readv(first.size(), first_chunk, 5s);
        if (!first_result) {
            done->set_value(std::unexpected(first_result.error()));
            co_return;
        }
        first_len = *first_result;
        if (first_len > first.size()) {
            done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
            co_return;
        }
        std::size_t gathered = 0;
        for (const fiber::mem::IoBufNode *node = first_chunk.front_node(); node != nullptr; node = node->next) {
            const std::size_t take = std::min(node->buf.readable(), first.size() - gathered);
            std::memcpy(first.data() + gathered, node->buf.readable_data(), take);
            gathered += take;
        }
        if (gathered != first_len || first_len != first.size()) {
            done->set_value(std::unexpected(fiber::common::IoErr::Invalid));
            co_return;
        }
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
    std::size_t rest_len = 0;
    {
        fiber::mem::IoBufChain rest_chunk;
        auto rest_result = co_await transport->readv(rest.size(), rest_chunk, 5s);
        if (!rest_result) {
            done->set_value(std::unexpected(rest_result.error()));
            co_return;
        }
        for (const fiber::mem::IoBufNode *node = rest_chunk.front_node(); node != nullptr; node = node->next) {
            const std::size_t take = std::min(node->buf.readable(), rest.size() - rest_len);
            std::memcpy(rest.data() + rest_len, node->buf.readable_data(), take);
            rest_len += take;
        }
    }

    std::string received(first.data(), first_len);
    received.append(rest.data(), rest_len);
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

    fiber::mem::IoBuf node = fiber::mem::IoBuf::allocate(payload.size());
    if (!node) {
        done->set_value(std::unexpected(fiber::common::IoErr::NoMem));
        co_return;
    }
    std::memcpy(node.writable_data(), payload.data(), payload.size());
    node.commit(payload.size());
    fiber::mem::IoBufChain write_chain;
    if (!write_chain.append(std::move(node))) {
        done->set_value(std::unexpected(fiber::common::IoErr::NoMem));
        co_return;
    }
    auto write_result = co_await transport->writev(write_chain, 5s);
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
        auto result = transport->try_writev(chain);
        if (!result) {
            if (result.error() == fiber::common::IoErr::WouldBlock) {
                // The pending group is retained; the peer drains continuously.
                co_await fiber::async::sleep(1ms);
                continue;
            }
            done->set_value(std::unexpected(result.error()));
            co_return;
        }
        if (*result == 0) {
            done->set_value(std::unexpected(fiber::common::IoErr::ConnReset));
            co_return;
        }
        records.push_back(*result);
    }
    (void) co_await transport->shutdown(5s);
    done->set_value(std::move(records));
    co_return;
}

// A polling writer (no write subscription) on a small send buffer: each
// try_writev reports its whole sealed batch, backs off with WouldBlock while
// that batch drains on its own, and every byte arrives in order.
TEST(TlsStreamFdTest, TlsTransportTryWritevBacksOffWhileTheSealedBatchDrains) {
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

// ---------------------------------------------------------------------------
// Dynamic credential ownership (issue #41): a configure callback hands the
// published identity over through add_credential and the publisher retires
// its own handle right away — the handshake alone must keep the identity
// alive until it stops reading it. Each test keeps a probe handle to the same
// material and watches use_count: 2 while the handshake retains it, 1 after.
// ---------------------------------------------------------------------------

fiber::net::TlsCredential make_identity(const std::string &cert_path, const std::string &key_path) {
    fiber::net::TlsCredentialOptions options{};
    options.certificate_chain = fiber::net::TlsPemSource::from_file(cert_path);
    options.private_key = fiber::net::TlsPemSource::from_file(key_path);
    auto credential = fiber::net::TlsCredential::create(options);
    return credential ? std::move(*credential) : fiber::net::TlsCredential{};
}

bool retained_beyond_probe(const fiber::net::TlsCredential &probe) { return probe.use_count() > 1; }

struct RotatingSelector {
    enum class Mode : std::uint8_t {
        Owned, // hand a copy over, then retire the publisher's handle
        OwnedThenFail, // move the handle over, then fail the callback
        Replace, // owned A → borrowed static → owned B → clear → borrowed static
    };

    Mode mode = Mode::Owned;
    fiber::net::TlsCredential published; // the publisher's handle
    fiber::net::TlsCredential published_second; // Replace only
    const fiber::net::TlsCredential *static_credential = nullptr; // Replace only
    const fiber::net::TlsCredential *probe = nullptr; // Replace only: the test's handle to `published`
    const fiber::net::TlsCredential *probe_second = nullptr; // Replace only
    std::atomic_bool selected{false};
    // Replace only: what the callback observed right after each step.
    bool released_by_borrowed_replace = false;
    bool released_by_clear = false;
    bool callback_ok = false;
};

fiber::common::IoErr select_and_retire(void *ctx, fiber::net::TlsServerHandshakeConfig &config,
                                       const fiber::tls::TlsClientHelloView &) noexcept {
    auto *selector = static_cast<RotatingSelector *>(ctx);
    selector->selected.store(true, std::memory_order_release);
    switch (selector->mode) {
        case RotatingSelector::Mode::Owned: {
            const fiber::common::IoErr error = config.add_credential(selector->published);
            selector->published.reset();
            return error;
        }
        case RotatingSelector::Mode::OwnedThenFail:
            if (config.add_credential(std::move(selector->published)) != fiber::common::IoErr::None) {
                return fiber::common::IoErr::Invalid;
            }
            return fiber::common::IoErr::Permission;
        case RotatingSelector::Mode::Replace: {
            if (config.add_credential(std::move(selector->published)) != fiber::common::IoErr::None ||
                !retained_beyond_probe(*selector->probe) ||
                config.add_borrowed_credential(*selector->static_credential) != fiber::common::IoErr::None) {
                return fiber::common::IoErr::Invalid;
            }
            selector->released_by_borrowed_replace = !retained_beyond_probe(*selector->probe);
            if (config.add_credential(std::move(selector->published_second)) != fiber::common::IoErr::None ||
                !retained_beyond_probe(*selector->probe_second) ||
                config.clear_credentials() != fiber::common::IoErr::None) {
                return fiber::common::IoErr::Invalid;
            }
            selector->released_by_clear = !retained_beyond_probe(*selector->probe_second);
            selector->callback_ok = true;
            return config.add_borrowed_credential(*selector->static_credential);
        }
    }
    return fiber::common::IoErr::Invalid;
}

fiber::net::TlsServerParam make_rotating_server_param(RotatingSelector &selector) {
    fiber::net::TlsServerParam param{};
    param.configure_callback = &select_and_retire;
    param.configure_ctx = &selector;
    return param;
}

bool set_nonblocking(int fd, bool enabled) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}

bool wait_fd_readable(int fd, int timeout_ms) {
    pollfd watched{.fd = fd, .events = POLLIN, .revents = 0};
    return ::poll(&watched, 1, timeout_ms) == 1 && (watched.revents & POLLIN) != 0;
}

// Sends ClientHello #1 offering a P-256 share only — the server prefers
// X25519, so it answers with a HelloRetryRequest — and returns once that HRR
// is readable. The server has then run its configure callback and parked
// waiting for ClientHello #2, with Certificate/CertificateVerify still ahead.
bool client_send_first_hello_and_await_hrr(BsslSocketClient &client, int fd) {
    if (SSL_set1_curves_list(client.ssl, "P-256:X25519") != 1 || !set_nonblocking(fd, true)) {
        return false;
    }
    const int ret = SSL_do_handshake(client.ssl);
    if (ret == 1 || SSL_get_error(client.ssl, ret) != SSL_ERROR_WANT_READ) {
        return false;
    }
    return wait_fd_readable(fd, 5000);
}

// Drives the non-blocking client to handshake completion.
bool client_finish_handshake(BsslSocketClient &client, int fd) {
    for (int i = 0; i < 100; ++i) {
        const int ret = SSL_do_handshake(client.ssl);
        if (ret == 1) {
            return set_nonblocking(fd, false);
        }
        if (SSL_get_error(client.ssl, ret) != SSL_ERROR_WANT_READ || !wait_fd_readable(fd, 5000)) {
            return false;
        }
    }
    return false;
}

TEST(TlsStreamFdTest, RetiredDynamicCredentialSurvivesHelloRetryRequest) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_owned_hrr", kSelfSignedCertPem);
        TempFile key("key_owned_hrr", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        RotatingSelector selector{};
        selector.published = make_identity(cert.path, key.path);
        ASSERT_FALSE(selector.published.empty());
        const fiber::net::TlsCredential probe = selector.published;
        fiber::net::TlsServerParam param = make_rotating_server_param(selector);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        arm_receive_timeout(fds[1]);

        fiber::event::EventLoopGroup group(1);
        group.start();

        std::unique_ptr<BsslSocketClient> client;
        ASSERT_TRUE(BsslSocketClient::make(false, false, client));
        client->attach_fd(fds[1]);

        auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[0]);
        std::promise<fiber::common::IoResult<std::string>> server_promise;
        auto server_future = server_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() { return run_tls_server(server_stream, param, &server_promise); });

        ASSERT_TRUE(client_send_first_hello_and_await_hrr(*client, fds[1]));
        EXPECT_TRUE(selector.selected.load(std::memory_order_acquire));
        EXPECT_TRUE(selector.published.empty()) << "the publisher retired its handle";
        EXPECT_EQ(probe.use_count(), 2u) << "the parked handshake must retain the identity";

        ASSERT_TRUE(client_finish_handshake(*client, fds[1]));
        EXPECT_EQ(SSL_used_hello_retry_request(client->ssl), 1);
        EXPECT_TRUE(client->round_trip("ping", "pong"));
        ASSERT_EQ(server_future.wait_for(5s), std::future_status::ready);
        auto served = server_future.get();
        ASSERT_TRUE(served);
        EXPECT_EQ(*served, "ping");
        EXPECT_EQ(probe.use_count(), 1u) << "released once the handshake ended";

        (void) SSL_shutdown(client->ssl);
        ::close(fds[1]);
        std::promise<void> close_done;
        auto close_future = close_done.get_future();
        fiber::async::spawn(group.at(0), [&]() { return close_tls_streams(server_stream, nullptr, &close_done); });
        ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);

        group.stop();
        group.join();
    });
}

TEST(TlsStreamFdTest, CanceledHandshakeReleasesRetainedCredential) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_owned_cancel", kSelfSignedCertPem);
        TempFile key("key_owned_cancel", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        RotatingSelector selector{};
        selector.published = make_identity(cert.path, key.path);
        ASSERT_FALSE(selector.published.empty());
        const fiber::net::TlsCredential probe = selector.published;
        fiber::net::TlsServerParam param = make_rotating_server_param(selector);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);

        fiber::event::EventLoopGroup group(1);
        group.start();

        std::unique_ptr<BsslSocketClient> client;
        ASSERT_TRUE(BsslSocketClient::make(false, false, client));
        client->attach_fd(fds[1]);

        auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[0]);
        std::promise<fiber::common::IoErr> server_promise;
        auto server_future = server_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return await_handshake_result(server_stream, param, &server_promise); });

        // The server is parked awaiting ClientHello #2 with the identity
        // retained; closing it cancels the handshake mid-flight.
        ASSERT_TRUE(client_send_first_hello_and_await_hrr(*client, fds[1]));
        EXPECT_EQ(probe.use_count(), 2u);

        std::promise<void> close_promise;
        auto close_future = close_promise.get_future();
        fiber::async::spawn(group.at(0), [&]() { return close_parked_handshake(server_stream, &close_promise); });
        ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);
        ASSERT_EQ(server_future.wait_for(2s), std::future_status::ready);
        EXPECT_EQ(server_future.get(), fiber::common::IoErr::Canceled);
        EXPECT_EQ(probe.use_count(), 1u);

        ::close(fds[1]);
        group.stop();
        group.join();
    });
}

TEST(TlsStreamFdTest, FailedConfigureCallbackReleasesRetainedCredential) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_owned_fail", kSelfSignedCertPem);
        TempFile key("key_owned_fail", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        RotatingSelector selector{};
        selector.mode = RotatingSelector::Mode::OwnedThenFail;
        selector.published = make_identity(cert.path, key.path);
        ASSERT_FALSE(selector.published.empty());
        const fiber::net::TlsCredential probe = selector.published;
        fiber::net::TlsServerParam param = make_rotating_server_param(selector);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        arm_receive_timeout(fds[1]);

        fiber::event::EventLoopGroup group(1);
        group.start();

        std::unique_ptr<BsslSocketClient> client;
        ASSERT_TRUE(BsslSocketClient::make(false, false, client));
        client->attach_fd(fds[1]);

        auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), fds[0]);
        std::promise<fiber::common::IoErr> server_promise;
        auto server_future = server_promise.get_future();
        fiber::async::spawn(group.at(0),
                            [&]() { return await_handshake_result(server_stream, param, &server_promise); });

        EXPECT_FALSE(client->handshake());
        ASSERT_EQ(server_future.wait_for(5s), std::future_status::ready);
        EXPECT_EQ(server_future.get(), fiber::common::IoErr::Permission);
        EXPECT_TRUE(selector.selected.load(std::memory_order_acquire));
        EXPECT_EQ(probe.use_count(), 1u);

        ::close(fds[1]);
        std::promise<void> close_done;
        auto close_future = close_done.get_future();
        fiber::async::spawn(group.at(0), [&]() { return close_tls_streams(server_stream, nullptr, &close_done); });
        ASSERT_EQ(close_future.wait_for(2s), std::future_status::ready);

        group.stop();
        group.join();
    });
}

TEST(TlsStreamFdTest, ReplacingOrClearingCredentialReleasesEarlierOwner) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_owned_replace", kSelfSignedCertPem);
        TempFile key("key_owned_replace", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);

        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        RotatingSelector selector{};
        selector.mode = RotatingSelector::Mode::Replace;
        selector.published = make_identity(cert.path, key.path);
        selector.published_second = make_identity(cert.path, key.path);
        ASSERT_FALSE(selector.published.empty());
        ASSERT_FALSE(selector.published_second.empty());
        const fiber::net::TlsCredential probe_first = selector.published;
        const fiber::net::TlsCredential probe_second = selector.published_second;
        selector.static_credential = &tls_pair->server_credential;
        selector.probe = &probe_first;
        selector.probe_second = &probe_second;
        fiber::net::TlsServerParam param = make_rotating_server_param(selector);

        int fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
        arm_receive_timeout(fds[1]);

        fiber::event::EventLoopGroup group(1);
        group.start();

        std::unique_ptr<BsslSocketClient> client;
        ASSERT_TRUE(BsslSocketClient::make(false, false, client));
        client->attach_fd(fds[1]);

        // The borrowed static credential ends up serving the handshake.
        EXPECT_EQ(run_server_against_bssl_client(group, fds[0], param, *client, fds[1], "ping", false), "ping");
        EXPECT_TRUE(selector.callback_ok);
        EXPECT_TRUE(selector.released_by_borrowed_replace);
        EXPECT_TRUE(selector.released_by_clear);
        EXPECT_EQ(probe_first.use_count(), 1u);
        EXPECT_EQ(probe_second.use_count(), 1u);

        group.stop();
        group.join();
    });
}

// =====================================================================
// connected-phase wire buffering (feature/tls/12)
// =====================================================================

// How the client→server bytes reach the server's socket.
enum class WirePath {
    Direct, // one socketpair
    // Relayed in 1..7-byte pieces (plus an occasional larger one) with a
    // pause between them: records — headers included — reach the reader
    // across many wire reads.
    TinyPieces,
    // Relayed with a pause before every read: bytes the client wrote back to
    // back (its Finished and the app data behind it) arrive in one write.
    Gathered,
};

void relay_bytes(int src, int dst, WirePath path) {
    static constexpr std::size_t kPieces[] = {1, 4, 2, 7, 3, 5, 6, 700};
    std::vector<std::uint8_t> buf(64 * 1024);
    std::size_t next_piece = 0;
    for (;;) {
        if (path == WirePath::Gathered) {
            std::this_thread::sleep_for(50ms);
        }
        const ssize_t got = ::read(src, buf.data(), buf.size());
        if (got <= 0) {
            break;
        }
        std::size_t off = 0;
        while (off < static_cast<std::size_t>(got)) {
            std::size_t len = static_cast<std::size_t>(got) - off;
            if (path == WirePath::TinyPieces) {
                len = std::min(len, kPieces[next_piece++ % std::size(kPieces)]);
            }
            const ssize_t put = ::write(dst, buf.data() + off, len);
            if (put <= 0) {
                return;
            }
            off += static_cast<std::size_t>(put);
            if (path == WirePath::TinyPieces) {
                std::this_thread::sleep_for(20us);
            }
        }
    }
    ::shutdown(dst, SHUT_WR);
}

// The relay's two directions and its socket ends. Destruction unblocks and
// joins the threads even when an assertion bails out early (a joinable
// std::thread would terminate the process).
struct WireRelay {
    std::thread up; // client → server, along the test's WirePath
    std::thread down; // server → client, direct
    int fds[2] = {-1, -1}; // [0]: the server socketpair's end, [1]: the client's

    WireRelay() = default;
    WireRelay(const WireRelay &) = delete;
    WireRelay &operator=(const WireRelay &) = delete;

    ~WireRelay() {
        for (const int fd: fds) {
            if (fd >= 0) {
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        if (up.joinable()) {
            up.join();
        }
        if (down.joinable()) {
            down.join();
        }
        for (const int fd: fds) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    }
};

std::string byte_pattern(std::size_t len, std::uint8_t seed) {
    std::string out(len, '\0');
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<char>(seed + i * 31 + (i >> 8));
    }
    return out;
}

struct StreamReadOutcome {
    fiber::common::IoResult<std::string> data;
    bool pending_after_handshake = false; // leftover app data opened at HandshakeDone
};

DetachedTask handshake_and_read_exact(fiber::net::detail::TlsStreamFd *stream, const fiber::net::TlsServerParam &param,
                                      std::size_t total, std::size_t read_size, std::promise<StreamReadOutcome> *done) {
    StreamReadOutcome outcome;
    auto handshake_result = co_await stream->handshake(param, 5s);
    if (!handshake_result) {
        outcome.data = std::unexpected(handshake_result.error());
        done->set_value(std::move(outcome));
        co_return;
    }
    outcome.pending_after_handshake = stream->has_pending_read();
    std::string &out = *outcome.data;
    while (out.size() < total) {
        fiber::mem::IoBufChain chain;
        auto read_result = co_await stream->readv(read_size, chain, 10s);
        if (!read_result || *read_result == 0) {
            outcome.data = std::unexpected(read_result ? fiber::common::IoErr::ConnReset : read_result.error());
            break;
        }
        for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
            out.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
        }
    }
    done->set_value(std::move(outcome));
}

DetachedTask handshake_and_write_pieces(fiber::net::detail::TlsStreamFd *stream,
                                        const fiber::net::TlsClientParam &param, const std::vector<std::string> *pieces,
                                        std::promise<fiber::common::IoErr> *done) {
    auto handshake_result = co_await stream->handshake(param, 5s);
    if (!handshake_result) {
        done->set_value(handshake_result.error());
        co_return;
    }
    for (const std::string &piece: *pieces) {
        auto written = co_await tls_poll_write(*stream, piece.data(), piece.size());
        if (!written) {
            done->set_value(written.error());
            co_return;
        }
    }
    done->set_value(fiber::common::IoErr::None);
}

DetachedTask close_tls_stream(fiber::net::detail::TlsStreamFd *stream, std::promise<void> *done) {
    stream->close();
    delete stream;
    done->set_value();
    co_return;
}

// Client (loop 1) writes `pieces`, the server (loop 0) reads them back with
// `read_size` per readv; the bytes must match exactly.
void check_wire_transfer(TestTlsPair &tls_pair, const std::vector<std::string> &pieces, std::size_t read_size,
                         WirePath path, bool expect_piggyback) {
    std::string expected;
    for (const std::string &piece: pieces) {
        expected += piece;
    }

    int server_fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, server_fds), 0);
    const int server_fd = server_fds[0];
    int client_fd = server_fds[1];
    WireRelay relay; // outlives the loop group (declared first, destroyed last)
    if (path != WirePath::Direct) {
        int client_fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, client_fds), 0);
        client_fd = client_fds[0];
        relay.fds[0] = server_fds[1];
        relay.fds[1] = client_fds[1];
        relay.up = std::thread(relay_bytes, relay.fds[1], relay.fds[0], path);
        relay.down = std::thread(relay_bytes, relay.fds[0], relay.fds[1], WirePath::Direct);
    }

    fiber::event::EventLoopGroup group(2);
    group.start();
    auto *server_stream = new fiber::net::detail::TlsStreamFd(group.at(0), server_fd);
    auto *client_stream = new fiber::net::detail::TlsStreamFd(group.at(1), client_fd);

    std::promise<StreamReadOutcome> server_promise;
    std::promise<fiber::common::IoErr> client_promise;
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return handshake_and_read_exact(server_stream, tls_pair.server_options, expected.size(), read_size,
                                        &server_promise);
    });
    fiber::async::spawn(group.at(1), [&]() {
        return handshake_and_write_pieces(client_stream, tls_pair.client_options, &pieces, &client_promise);
    });
    ASSERT_EQ(server_future.wait_for(20s), std::future_status::ready);
    ASSERT_EQ(client_future.wait_for(20s), std::future_status::ready);
    StreamReadOutcome server_outcome = server_future.get();
    const fiber::common::IoErr client_err = client_future.get();

    std::promise<void> server_close_promise;
    std::promise<void> client_close_promise;
    auto server_close_future = server_close_promise.get_future();
    auto client_close_future = client_close_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() { return close_tls_stream(server_stream, &server_close_promise); });
    fiber::async::spawn(group.at(1), [&]() { return close_tls_stream(client_stream, &client_close_promise); });
    ASSERT_EQ(server_close_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(client_close_future.wait_for(2s), std::future_status::ready);
    group.stop();
    group.join();

    EXPECT_EQ(client_err, fiber::common::IoErr::None);
    ASSERT_TRUE(server_outcome.data) << static_cast<int>(server_outcome.data.error());
    EXPECT_EQ(server_outcome.data->size(), expected.size());
    EXPECT_TRUE(*server_outcome.data == expected);
    if (expect_piggyback) {
        EXPECT_TRUE(server_outcome.pending_after_handshake);
    }
}

// Bulk reads fill each wire buffer and end mid-record: the incomplete tail is
// carried into the next buffer — with a small read size (one record + a
// partial per 20 KiB buffer) and a large one (64 KiB buffers).
TEST(TlsStreamFdTest, BulkReadCarriesIncompleteRecordsAcrossWireBuffers) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_bulk", kSelfSignedCertPem);
        TempFile key("key_bulk", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);
        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        const std::vector<std::string> pieces{byte_pattern(1 << 20, 3)};
        for (const std::size_t read_size: {std::size_t{4096}, std::size_t{65536}}) {
            SCOPED_TRACE(read_size);
            check_wire_transfer(*tls_pair, pieces, read_size, WirePath::Direct, false);
            if (::testing::Test::HasFatalFailure()) {
                return;
            }
        }
    });
}

// Records trickle in a few bytes per read: each one continues in its wire
// buffer's tailroom until the buffer fills, then carries into a fresh one.
TEST(TlsStreamFdTest, TinySegmentsReassembleRecordsAcrossReads) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_tiny", kSelfSignedCertPem);
        TempFile key("key_tiny", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);
        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        const std::vector<std::string> pieces{byte_pattern(1, 1),    byte_pattern(100, 2),   byte_pattern(2000, 3),
                                              byte_pattern(3900, 4), byte_pattern(16984, 5), byte_pattern(7000, 6)};
        check_wire_transfer(*tls_pair, pieces, 4096, WirePath::TinyPieces, false);
    });
}

// App data written right behind the client's Finished lands in the server's
// last handshake read: the engine's leftover becomes the first wire buffer
// and its record is opened at HandshakeDone, before any read.
TEST(TlsStreamFdTest, AppDataBehindClientFinishedIsReadableAfterHandshake) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_piggyback", kSelfSignedCertPem);
        TempFile key("key_piggyback", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);
        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);

        const std::vector<std::string> pieces{byte_pattern(3000, 7)};
        check_wire_transfer(*tls_pair, pieces, 4096, WirePath::Gathered, true);
    });
}

// =====================================================================
// buffered plaintext is read readiness
// =====================================================================

enum class PendingPlaintextProbe {
    Observe,
    // Subscribes for read with the plaintext buffered: a contract violation.
    Subscribe,
};

// What the server sees with plaintext buffered behind a drained socket, then
// once it is read out.
struct PendingPlaintextOutcome {
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    bool pending = false;
    bool ready = false;
    fiber::common::IoErr wait = fiber::common::IoErr::Unknown;
    bool drained_ready = true;
    fiber::common::IoErr drained_wait = fiber::common::IoErr::None;
    std::string received;
};

void ignore_ready(void *, fiber::common::IoErr) noexcept {}

fiber::async::Task<fiber::common::IoErr> read_append(fiber::net::detail::TlsStreamFd &stream, std::size_t size,
                                                     std::string &out) {
    fiber::mem::IoBufChain chain;
    auto read_result = co_await stream.readv(size, chain, 5s);
    if (!read_result) {
        co_return read_result.error();
    }
    if (*read_result == 0) {
        co_return fiber::common::IoErr::ConnReset;
    }
    for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
        out.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
    }
    co_return fiber::common::IoErr::None;
}

DetachedTask probe_pending_plaintext(fiber::net::detail::TlsStreamFd *stream, const fiber::net::TlsServerParam *param,
                                     std::size_t total, PendingPlaintextProbe probe, bool *server_done,
                                     PendingPlaintextOutcome *outcome) {
    auto handshake_result = co_await stream->handshake(*param, 5s);
    fiber::common::IoErr err = handshake_result ? fiber::common::IoErr::None : handshake_result.error();
    if (err == fiber::common::IoErr::None) {
        // The peer's one record lands whole in a wire read that comes up
        // short (the socket drained); a quarter of it is delivered.
        err = co_await read_append(*stream, total / 4, outcome->received);
    }
    if (err == fiber::common::IoErr::None) {
        outcome->pending = stream->has_pending_read();
        outcome->ready = stream->read_ready();
        if (probe == PendingPlaintextProbe::Subscribe) {
            (void) stream->set_read_callback(&ignore_ready, nullptr);
        }
        // No socket edge can come: the peer stays silent until server_done.
        auto waited = co_await stream->wait_readable(1s);
        outcome->wait = waited ? fiber::common::IoErr::None : waited.error();
        while (err == fiber::common::IoErr::None && outcome->received.size() < total) {
            err = co_await read_append(*stream, total, outcome->received);
        }
    }
    if (err == fiber::common::IoErr::None) {
        outcome->drained_ready = stream->read_ready();
        auto waited = co_await stream->wait_readable(50ms);
        outcome->drained_wait = waited ? fiber::common::IoErr::None : waited.error();
    }
    outcome->err = err;
    *server_done = true;
}

DetachedTask write_record_and_park(fiber::net::detail::TlsStreamFd *client, fiber::net::detail::TlsStreamFd *server,
                                   const fiber::net::TlsClientParam *param, const std::string *payload,
                                   const bool *server_done, fiber::common::IoErr *client_err) {
    auto handshake_result = co_await client->handshake(*param, 5s);
    *client_err = handshake_result ? fiber::common::IoErr::None : handshake_result.error();
    if (*client_err == fiber::common::IoErr::None) {
        auto written = co_await tls_poll_write(*client, payload->data(), payload->size());
        *client_err = written ? fiber::common::IoErr::None : written.error();
    }
    while (!*server_done) {
        co_await fiber::async::sleep(1ms);
    }
    // The client closes first: the server's close_notify then meets a gone
    // peer and stays unflushed.
    client->close();
    server->close();
    fiber::event::EventLoop::current().stop();
}

// Both ends on one loop, so the sequencing flag needs no synchronization.
// The streams are closed on the loop and destroyed after it stopped.
void run_pending_plaintext_probe(TestTlsPair &tls_pair, const std::string &payload, PendingPlaintextProbe probe,
                                 PendingPlaintextOutcome &outcome, fiber::common::IoErr &client_err) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
    fiber::event::EventLoop loop;
    fiber::net::detail::TlsStreamFd server(loop, fds[0]);
    fiber::net::detail::TlsStreamFd client(loop, fds[1]);
    bool server_done = false;
    fiber::async::spawn(loop, [&]() {
        return probe_pending_plaintext(&server, &tls_pair.server_options, payload.size(), probe, &server_done,
                                       &outcome);
    });
    fiber::async::spawn(loop, [&]() {
        return write_record_and_park(&client, &server, &tls_pair.client_options, &payload, &server_done, &client_err);
    });
    loop.run();
}

// Plaintext opened from an already-consumed wire read is never announced by
// a socket edge: read_ready() and wait_readable() report it, and stop doing
// so once it is read out.
TEST(TlsStreamFdTest, BufferedPlaintextIsReadReadiness) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert_pending", kSelfSignedCertPem);
    TempFile key("key_pending", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);
    auto tls_pair = create_tls_pair(cert.path, key.path);
    ASSERT_TRUE(tls_pair);

    const std::string payload = byte_pattern(4096, 11);
    PendingPlaintextOutcome outcome;
    fiber::common::IoErr client_err = fiber::common::IoErr::Unknown;
    run_pending_plaintext_probe(*tls_pair, payload, PendingPlaintextProbe::Observe, outcome, client_err);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    EXPECT_EQ(client_err, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.err, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.pending);
    EXPECT_TRUE(outcome.ready);
    EXPECT_EQ(outcome.wait, fiber::common::IoErr::None);
    EXPECT_FALSE(outcome.drained_ready);
    EXPECT_EQ(outcome.drained_wait, fiber::common::IoErr::TimedOut);
    EXPECT_TRUE(outcome.received == payload);
}

// A read subscription over buffered plaintext would never fire: rejected
// like a subscription on a Ready fd.
TEST(TlsStreamFdDeathTest, ReadSubscriptionOverBufferedPlaintextAsserts) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert_pending_death", kSelfSignedCertPem);
    TempFile key("key_pending_death", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);
    auto tls_pair = create_tls_pair(cert.path, key.path);
    ASSERT_TRUE(tls_pair);

    const std::string payload = byte_pattern(4096, 12);
    EXPECT_DEATH(
            {
                PendingPlaintextOutcome outcome;
                fiber::common::IoErr client_err = fiber::common::IoErr::Unknown;
                run_pending_plaintext_probe(*tls_pair, payload, PendingPlaintextProbe::Subscribe, outcome, client_err);
            },
            "FIBER_ASSERT failed: !has_pending_read");
}

DetachedTask handshake_and_flag(fiber::net::detail::TlsStreamFd *stream, const fiber::net::TlsServerParam *param,
                                fiber::common::IoErr *err, bool *done) {
    auto handshake_result = co_await stream->handshake(*param, 5s);
    *err = handshake_result ? fiber::common::IoErr::None : handshake_result.error();
    *done = true;
}

// close() drops the output its best-effort drain could not send (here the
// close_notify a gone peer refuses) on the loop, so the closed stream holds
// no loop-bound buffers and may be destroyed after the loop stopped.
TEST(TlsStreamFdTest, CloseReleasesUnflushedOutput) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert_close", kSelfSignedCertPem);
    TempFile key("key_close", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);
    auto tls_pair = create_tls_pair(cert.path, key.path);
    ASSERT_TRUE(tls_pair);

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
    const std::string payload = byte_pattern(100, 13);
    fiber::common::IoErr server_err = fiber::common::IoErr::Unknown;
    fiber::common::IoErr client_err = fiber::common::IoErr::Unknown;
    {
        fiber::event::EventLoop loop;
        fiber::net::detail::TlsStreamFd server(loop, fds[0]);
        fiber::net::detail::TlsStreamFd client(loop, fds[1]);
        bool server_done = false;
        fiber::async::spawn(loop, [&]() {
            return handshake_and_flag(&server, &tls_pair->server_options, &server_err, &server_done);
        });
        fiber::async::spawn(loop, [&]() {
            return write_record_and_park(&client, &server, &tls_pair->client_options, &payload, &server_done,
                                         &client_err);
        });
        loop.run();
        EXPECT_FALSE(server.valid());
        EXPECT_FALSE(client.valid());
    } // both destroyed here, with no loop running on this thread
    EXPECT_EQ(server_err, fiber::common::IoErr::None);
    EXPECT_EQ(client_err, fiber::common::IoErr::None);
}

// =====================================================================
// sealed is accepted: the self-draining write side (feature/tls/14)
// =====================================================================

using StreamBody = std::function<fiber::async::Task<void>(fiber::net::detail::TlsStreamFd &)>;

struct DrainPairRun {
    fiber::common::IoErr server_handshake = fiber::common::IoErr::Unknown;
    fiber::common::IoErr client_handshake = fiber::common::IoErr::Unknown;
    int finished = 0;
};

template<class Param>
DetachedTask handshake_then(fiber::net::detail::TlsStreamFd *stream, const Param *param, const StreamBody *body,
                            fiber::common::IoErr *handshake_err, int *finished) {
    auto handshake_result = co_await stream->handshake(*param, 5s);
    *handshake_err = handshake_result ? fiber::common::IoErr::None : handshake_result.error();
    if (handshake_result) {
        co_await (*body)(*stream);
    }
    ++*finished;
}

DetachedTask close_pair_when_done(fiber::net::detail::TlsStreamFd *server, fiber::net::detail::TlsStreamFd *client,
                                  const int *finished) {
    const auto deadline = fiber::event::EventLoop::current().now() + 10s;
    while (*finished < 2 && fiber::event::EventLoop::current().now() < deadline) {
        co_await fiber::async::sleep(1ms);
    }
    client->close();
    server->close();
    fiber::event::EventLoop::current().stop();
}

// Both ends on one loop; each body runs once its own handshake is done. The
// client's send buffer is tiny by default, so a 64 KiB batch cannot leave in
// one write while the server is not reading; 0 keeps the system default. Both
// streams are closed on the loop and destroyed after it stopped.
void run_drain_pair(TestTlsPair &tls_pair, const StreamBody &server_body, const StreamBody &client_body,
                    DrainPairRun &run, int client_send_buffer = 4096) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds), 0);
    if (client_send_buffer > 0) {
        ASSERT_EQ(::setsockopt(fds[1], SOL_SOCKET, SO_SNDBUF, &client_send_buffer, sizeof(client_send_buffer)), 0);
    }
    fiber::event::EventLoop loop;
    fiber::net::detail::TlsStreamFd server(loop, fds[0]);
    fiber::net::detail::TlsStreamFd client(loop, fds[1]);
    fiber::async::spawn(loop, [&]() {
        return handshake_then(&server, &tls_pair.server_options, &server_body, &run.server_handshake, &run.finished);
    });
    fiber::async::spawn(loop, [&]() {
        return handshake_then(&client, &tls_pair.client_options, &client_body, &run.client_handshake, &run.finished);
    });
    fiber::async::spawn(loop, [&]() { return close_pair_when_done(&server, &client, &run.finished); });
    loop.run();
}

template<class Predicate>
fiber::async::Task<bool> poll_until(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = fiber::event::EventLoop::current().now() + timeout;
    while (!predicate()) {
        if (fiber::event::EventLoop::current().now() >= deadline) {
            co_return false;
        }
        co_await fiber::async::sleep(1ms);
    }
    co_return true;
}

fiber::async::Task<fiber::common::IoErr> read_exact(fiber::net::detail::TlsStreamFd &stream, std::size_t total,
                                                    std::string &out) {
    while (out.size() < total) {
        const fiber::common::IoErr err = co_await read_append(stream, total - out.size(), out);
        if (err != fiber::common::IoErr::None) {
            co_return err;
        }
    }
    co_return fiber::common::IoErr::None;
}

constexpr std::size_t kDrainBatch = 64 * 1024; // TlsStreamFd's write batch, sealed from 4 KiB nodes

// The client's first write: one batch out of 64 x 4 KiB, which the tiny send
// buffer cannot take whole.
fiber::common::IoResult<std::size_t> write_blocked_batch(fiber::net::detail::TlsStreamFd &stream,
                                                         fiber::mem::IoBufChain &chain, std::string &expected) {
    expected = build_distinct_chain(fiber::event::EventLoop::current().io_buf_node_pool(), chain,
                                    std::vector<std::size_t>(64, 4096));
    return stream.try_write(chain);
}

struct WriteReadyProbe {
    fiber::net::detail::TlsStreamFd *stream = nullptr;
    int calls = 0;
    fiber::common::IoErr err = fiber::common::IoErr::Unknown;
    bool pending = true;
    bool ready = false;
};

void on_write_ready_probe(void *ctx, fiber::common::IoErr err) noexcept {
    auto *probe = static_cast<WriteReadyProbe *>(ctx);
    ++probe->calls;
    probe->err = err;
    if (err == fiber::common::IoErr::None) {
        probe->pending = probe->stream->has_pending_write();
        probe->ready = probe->stream->write_ready();
    }
}

// Credential files and the pair built from them; setup failures skip the test.
class TlsStreamFdDrainTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(cert_.ok);
        ASSERT_TRUE(key_.ok);
        fiber::net::TlsCredentialOptions options{};
        options.certificate_chain = fiber::net::TlsPemSource::from_file(cert_.path);
        options.private_key = fiber::net::TlsPemSource::from_file(key_.path);
        auto credential = fiber::net::TlsCredential::create(options);
        ASSERT_TRUE(credential);
        tls_pair_.emplace(std::move(*credential));
    }

    SigpipeGuard sigpipe_guard_;
    TempFile cert_{"cert_drain", kSelfSignedCertPem};
    TempFile key_{"key_drain", kSelfSignedKeyPem};
    std::optional<TestTlsPair> tls_pair_;
};

using TlsStreamFdDrainDeathTest = TlsStreamFdDrainTest;

// try_write reports the sealed batch and consumes it although the socket took
// only part of it; until the rest is out every write backs off, and it gets
// out with no further call.
TEST_F(TlsStreamFdDrainTest, TryWriteAcceptsTheSealedBatchAndDrainsItAlone) {
    fiber::common::IoResult<std::size_t> first = std::unexpected(fiber::common::IoErr::Unknown);
    std::size_t left_in_chain = 0;
    bool pending = false;
    bool ready = true;
    fiber::common::IoErr same_chain = fiber::common::IoErr::None;
    fiber::common::IoErr other_chain = fiber::common::IoErr::None;
    bool drained = false;
    bool ready_after = false;
    std::string expected;
    std::string received;
    fiber::common::IoErr read_err = fiber::common::IoErr::Unknown;

    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        first = write_blocked_batch(stream, chain, expected);
        left_in_chain = chain.readable_bytes();
        pending = stream.has_pending_write();
        ready = stream.write_ready();
        auto again = stream.try_write(chain);
        same_chain = again ? fiber::common::IoErr::None : again.error();
        fiber::mem::IoBufChain other;
        auto empty = stream.try_write(other);
        other_chain = empty ? fiber::common::IoErr::None : empty.error();
        // No further call: the batch must drain while this side only waits.
        drained = co_await poll_until([&] { return !stream.has_pending_write(); }, 5s);
        ready_after = stream.write_ready();
    };
    const StreamBody server = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        read_err = co_await read_exact(stream, kDrainBatch, received);
    };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.server_handshake, fiber::common::IoErr::None);
    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, kDrainBatch);
    EXPECT_EQ(left_in_chain, 64U * 4096U - kDrainBatch);
    EXPECT_TRUE(pending);
    EXPECT_FALSE(ready);
    EXPECT_EQ(same_chain, fiber::common::IoErr::WouldBlock);
    EXPECT_EQ(other_chain, fiber::common::IoErr::WouldBlock);
    EXPECT_TRUE(drained);
    EXPECT_TRUE(ready_after);
    EXPECT_EQ(read_err, fiber::common::IoErr::None);
    EXPECT_TRUE(received == expected.substr(0, kDrainBatch));
}

// The TLS-level write subscriber is notified once, after the drain: never
// inline from the setter, never while sealed output is still pending.
TEST_F(TlsStreamFdDrainTest, WriteCallbackFiresOnlyOnceTheDrainIsDone) {
    WriteReadyProbe probe;
    fiber::common::IoErr subscribed = fiber::common::IoErr::Unknown;
    int calls_after_subscribe = -1;
    bool notified = false;
    std::string expected;
    std::string received;

    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        (void) write_blocked_batch(stream, chain, expected);
        probe.stream = &stream;
        subscribed = stream.set_write_callback(&on_write_ready_probe, &probe);
        calls_after_subscribe = probe.calls;
        notified = co_await poll_until([&] { return probe.calls > 0; }, 5s);
        co_await fiber::async::sleep(20ms); // no second notification without a new block
        (void) stream.clear_write_callback(&on_write_ready_probe, &probe);
    };
    const StreamBody server = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        co_await fiber::async::sleep(20ms);
        (void) co_await read_exact(stream, kDrainBatch, received);
    };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    EXPECT_EQ(subscribed, fiber::common::IoErr::None);
    EXPECT_EQ(calls_after_subscribe, 0);
    EXPECT_TRUE(notified);
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.err, fiber::common::IoErr::None);
    EXPECT_FALSE(probe.pending);
    EXPECT_TRUE(probe.ready);
    EXPECT_TRUE(received == expected.substr(0, kDrainBatch));
}

// close() mid-drain drops the sealed output and completes the TLS-level
// write subscriber once, with Canceled.
TEST_F(TlsStreamFdDrainTest, CloseDuringTheDrainCancelsTheWriteSubscriber) {
    WriteReadyProbe probe;
    bool pending_before = false;
    bool pending_after = true;
    std::string expected;

    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        (void) write_blocked_batch(stream, chain, expected);
        pending_before = stream.has_pending_write();
        probe.stream = &stream;
        (void) stream.set_write_callback(&on_write_ready_probe, &probe);
        stream.close();
        pending_after = stream.has_pending_write();
        co_return;
    };
    const StreamBody server = [](fiber::net::detail::TlsStreamFd &) -> fiber::async::Task<void> { co_return; };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    EXPECT_TRUE(pending_before);
    EXPECT_FALSE(pending_after);
    EXPECT_EQ(probe.calls, 1);
    EXPECT_EQ(probe.err, fiber::common::IoErr::Canceled);
}

// writev returns each batch only once it is on the wire, so a writev user
// never leaves sealed output behind.
TEST_F(TlsStreamFdDrainTest, WritevReturnsOnlyOnceItsBatchLeft) {
    std::vector<std::size_t> returns;
    bool pending_after_any = false;
    fiber::common::IoErr write_err = fiber::common::IoErr::None;
    std::string expected;
    std::string received;

    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        expected = build_distinct_chain(fiber::event::EventLoop::current().io_buf_node_pool(), chain,
                                        std::vector<std::size_t>(64, 4096));
        while (chain.readable_bytes() > 0) {
            auto written = co_await stream.writev(chain, 5s);
            if (!written) {
                write_err = written.error();
                co_return;
            }
            returns.push_back(*written);
            pending_after_any = pending_after_any || stream.has_pending_write();
        }
    };
    const StreamBody server = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        co_await fiber::async::sleep(20ms);
        (void) co_await read_exact(stream, 64 * 4096, received);
    };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    EXPECT_EQ(write_err, fiber::common::IoErr::None);
    EXPECT_EQ(returns, std::vector<std::size_t>(4, kDrainBatch));
    EXPECT_FALSE(pending_after_any);
    EXPECT_TRUE(received == expected);
}

// wait_writable waits for the drain, not the socket: it times out while the
// peer reads nothing, an abandoned wait (timeout_for) frees the slot, and it
// completes once the drain is done.
TEST_F(TlsStreamFdDrainTest, WaitWritableWaitsForTheDrainAndUnsubscribesWhenAbandoned) {
    fiber::common::IoErr own_timeout = fiber::common::IoErr::None;
    fiber::common::IoErr wrapped_timeout = fiber::common::IoErr::None;
    fiber::common::IoErr resubscribed = fiber::common::IoErr::Unknown;
    fiber::common::IoErr drained_wait = fiber::common::IoErr::Unknown;
    bool pending_after = true;
    bool may_read = false;
    std::string expected;
    std::string received;

    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        (void) write_blocked_batch(stream, chain, expected);
        auto waited = co_await stream.wait_writable(20ms);
        own_timeout = waited ? fiber::common::IoErr::None : waited.error();
        auto wrapped = co_await fiber::async::timeout_for([&]() { return stream.wait_writable(); }, 20ms);
        wrapped_timeout = wrapped ? fiber::common::IoErr::None : wrapped.error();
        WriteReadyProbe probe;
        probe.stream = &stream;
        resubscribed = stream.set_write_callback(&on_write_ready_probe, &probe);
        (void) stream.clear_write_callback(&on_write_ready_probe, &probe);
        may_read = true;
        auto drained = co_await stream.wait_writable(5s);
        drained_wait = drained ? fiber::common::IoErr::None : drained.error();
        pending_after = stream.has_pending_write();
    };
    const StreamBody server = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        (void) co_await poll_until([&] { return may_read; }, 5s);
        (void) co_await read_exact(stream, kDrainBatch, received);
    };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    EXPECT_EQ(own_timeout, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(wrapped_timeout, fiber::common::IoErr::TimedOut);
    EXPECT_EQ(resubscribed, fiber::common::IoErr::None);
    EXPECT_EQ(drained_wait, fiber::common::IoErr::None);
    EXPECT_FALSE(pending_after);
    EXPECT_TRUE(received == expected.substr(0, kDrainBatch));
}

// A graceful shutdown queues close_notify behind the batch still draining:
// the peer reads the whole batch, then EOF.
TEST_F(TlsStreamFdDrainTest, PollShutdownSendsCloseNotifyBehindTheDrain) {
    fiber::common::IoErr shutdown_err = fiber::common::IoErr::Unknown;
    bool blocked_on_write = false;
    bool eof = false;
    fiber::common::IoErr read_err = fiber::common::IoErr::None;
    std::string expected;
    std::string received;

    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        (void) write_blocked_batch(stream, chain, expected);
        for (;;) {
            fiber::event::IoEvent event = fiber::event::IoEvent::None;
            shutdown_err = stream.poll_shutdown(event);
            if (shutdown_err != fiber::common::IoErr::WouldBlock) {
                break;
            }
            blocked_on_write = event == fiber::event::IoEvent::Write;
            auto waited = co_await stream.wait_writable(5s);
            if (!waited) {
                shutdown_err = waited.error();
                break;
            }
        }
    };
    const StreamBody server = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        co_await fiber::async::sleep(20ms);
        for (;;) {
            fiber::mem::IoBufChain chain;
            auto read_result = co_await stream.readv(kDrainBatch, chain, 5s);
            if (!read_result) {
                read_err = read_result.error();
                co_return;
            }
            if (*read_result == 0) {
                eof = true;
                co_return;
            }
            for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
                received.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
            }
        }
    };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    EXPECT_EQ(shutdown_err, fiber::common::IoErr::None);
    EXPECT_TRUE(blocked_on_write);
    EXPECT_EQ(read_err, fiber::common::IoErr::None);
    EXPECT_TRUE(eof);
    EXPECT_TRUE(received == expected.substr(0, kDrainBatch));
}

// Handover is for writev users, whose output is always out by then: a stream
// still draining must not move loops.
TEST_F(TlsStreamFdDrainDeathTest, HandoverWithUndrainedOutputAsserts) {
    const StreamBody client = [](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        fiber::mem::IoBufChain chain;
        std::string expected;
        (void) write_blocked_batch(stream, chain, expected);
        (void) stream.detach_for_handover();
        co_return;
    };
    const StreamBody server = [](fiber::net::detail::TlsStreamFd &) -> fiber::async::Task<void> { co_return; };
    EXPECT_DEATH(
            {
                DrainPairRun run;
                run_drain_pair(*tls_pair_, server, client, run);
            },
            "FIBER_ASSERT failed: out_pending_");
}

// =====================================================================
// reads deliver everything buffered
// =====================================================================

using TlsStreamFdReadTest = TlsStreamFdDrainTest;

// A read takes everything one wire buffer opened, several records' worth,
// not one record per call: the client's three full records arrive in a
// single write, behind a server already parked on its read.
TEST_F(TlsStreamFdReadTest, ReadDeliversSeveralRecordsInOneCall) {
    constexpr std::size_t kRecordPlaintext = 16 * 1024;
    const std::string payload = byte_pattern(3 * kRecordPlaintext, 13);
    bool server_reading = false;
    fiber::common::IoErr write_err = fiber::common::IoErr::Unknown;
    fiber::common::IoResult<std::size_t> first_read = std::unexpected(fiber::common::IoErr::Unknown);
    std::string received;

    const StreamBody server = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        server_reading = true;
        fiber::mem::IoBufChain chain;
        first_read = co_await stream.readv(64 * 1024, chain, 5s);
        for (const fiber::mem::IoBufNode *node = chain.front_node(); node != nullptr; node = node->next) {
            received.append(reinterpret_cast<const char *>(node->buf.readable_data()), node->buf.readable());
        }
    };
    const StreamBody client = [&](fiber::net::detail::TlsStreamFd &stream) -> fiber::async::Task<void> {
        // Nothing may ride behind the client's Finished: write only once the
        // server reads from an empty socket.
        if (!co_await poll_until([&] { return server_reading; }, 5s)) {
            write_err = fiber::common::IoErr::TimedOut;
            co_return;
        }
        auto written = co_await tls_poll_write(stream, payload.data(), payload.size());
        write_err = written ? fiber::common::IoErr::None : written.error();
    };
    DrainPairRun run;
    run_drain_pair(*tls_pair_, server, client, run, 0);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());

    ASSERT_EQ(run.server_handshake, fiber::common::IoErr::None);
    ASSERT_EQ(run.client_handshake, fiber::common::IoErr::None);
    EXPECT_EQ(write_err, fiber::common::IoErr::None);
    ASSERT_TRUE(first_read) << static_cast<int>(first_read.error());
    EXPECT_EQ(*first_read, payload.size());
    EXPECT_TRUE(received == payload);
}


// =====================================================================
// batched write flush (feature/tls/13)
// =====================================================================

// Writes a chain of `sizes` nodes through a TLS pair (default socket buffers)
// and reports what each try_writev returned; the peer must read the bytes
// back unchanged. A sealed batch is only ever reported whole, so the
// sequence of returns is deterministic whatever WouldBlock does.
void write_chain_through_tls(::fiber::mem::IoBufNodePool &pool, const std::vector<std::size_t> &sizes,
                             PollWriteStats &stats) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert_batch", kSelfSignedCertPem);
    TempFile key("key_batch", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key.ok);
    auto tls_pair = create_tls_pair(cert.path, key.path);
    ASSERT_TRUE(tls_pair);

    int fds[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds), 0);
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
    const std::string expected = build_distinct_chain(pool, chain, sizes);

    std::promise<fiber::common::IoResult<std::string>> server_promise;
    std::promise<fiber::common::IoResult<PollWriteStats>> client_promise;
    auto server_future = server_promise.get_future();
    auto client_future = client_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() {
        return run_poll_transport_server(server_transport, tls_pair->server_options, expected.size(), &server_promise);
    });
    fiber::async::spawn(group.at(1), [&]() {
        return run_poll_transport_client(client_transport, tls_pair->client_options, std::move(chain), &client_promise);
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
    EXPECT_TRUE(*server_result == expected);
    stats = std::move(*client_result);
}

// H2's outbound shape — a 9-byte frame header node before each payload node.
// Every group coalesces a full record across the header/payload seam, and
// four of them go out per call instead of one.
TEST(TlsStreamFdTest, TlsTransportWritevBatchesH2ShapedChain) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        std::vector<std::size_t> sizes;
        for (int frame = 0; frame < 8; ++frame) {
            sizes.push_back(9);
            sizes.push_back(16384);
        }
        PollWriteStats stats;
        write_chain_through_tls(pool, sizes, stats);
        EXPECT_EQ(stats.calls, (std::vector<std::size_t>{65536, 65536, 72}));
    });
}

// Coalescing walks any number of nodes: 2000 ten-byte nodes fill whole
// records (a 16-iovec walk would seal 160-byte records).
TEST(TlsStreamFdTest, TlsTransportWritevCoalescesManySmallNodesIntoFullRecords) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        PollWriteStats stats;
        write_chain_through_tls(pool, std::vector<std::size_t>(2000, 10), stats);
        EXPECT_EQ(stats.calls, (std::vector<std::size_t>{20000}));
    });
}

// A large single node seals one batch per call, not the whole node at once:
// sealed-but-unwritten output stays bounded by a batch.
TEST(TlsStreamFdTest, TlsTransportWritevBatchesALargeNode) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        PollWriteStats stats;
        write_chain_through_tls(pool, {std::size_t{1} << 20}, stats);
        EXPECT_EQ(stats.calls, std::vector<std::size_t>(16, 65536));
    });
}

// Forwards src → dst unchanged and logs every TLS record crossing it as
// (outer content type, record length), parsing headers across reads.
void relay_logging_records(int src, int dst, std::vector<std::pair<std::uint8_t, std::size_t>> *records) {
    std::vector<std::uint8_t> buf(64 * 1024);
    std::vector<std::uint8_t> pending;
    for (;;) {
        const ssize_t got = ::read(src, buf.data(), buf.size());
        if (got <= 0) {
            break;
        }
        std::size_t off = 0;
        while (off < static_cast<std::size_t>(got)) {
            const ssize_t put = ::write(dst, buf.data() + off, static_cast<std::size_t>(got) - off);
            if (put <= 0) {
                return;
            }
            off += static_cast<std::size_t>(put);
        }
        pending.insert(pending.end(), buf.begin(), buf.begin() + got);
        std::size_t pos = 0;
        while (pending.size() - pos >= 5) {
            const std::size_t len = (static_cast<std::size_t>(pending[pos + 3]) << 8) | pending[pos + 4];
            if (pending.size() - pos < 5 + len) {
                break;
            }
            records->emplace_back(pending[pos], len);
            pos += 5 + len;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pos));
    }
    ::shutdown(dst, SHUT_WR);
}

// Record boundaries on the wire, not just per-call returns: H2-shaped
// [9][16 KiB] nodes, an oversized node and small trailing nodes fill 16 KiB
// records, and the oversized node's tail is coalesced with the trailing
// nodes instead of becoming a short record of its own. One write batch
// covers the first four records.
TEST(TlsStreamFdTest, TlsTransportWritevFillsRecordsAcrossSmallAndLargeNodes) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        SigpipeGuard sigpipe_guard;
        TempFile cert("cert_fill", kSelfSignedCertPem);
        TempFile key("key_fill", kSelfSignedKeyPem);
        ASSERT_TRUE(cert.ok);
        ASSERT_TRUE(key.ok);
        auto tls_pair = create_tls_pair(cert.path, key.path);
        ASSERT_TRUE(tls_pair);
        // TLS 1.3: every record past the handshake is outer type 23 and
        // carries plaintext of its length minus 17 (inner type + tag).
        tls_pair->client_options.min_version = 0x0304;
        tls_pair->client_options.max_version = 0x0304;

        int server_fds[2] = {-1, -1};
        int client_fds[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, server_fds), 0);
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, client_fds), 0);
        std::vector<std::pair<std::uint8_t, std::size_t>> records; // client → server
        WireRelay relay;
        relay.fds[0] = server_fds[1];
        relay.fds[1] = client_fds[1];
        relay.up = std::thread(relay_logging_records, relay.fds[1], relay.fds[0], &records);
        relay.down = std::thread(relay_bytes, relay.fds[0], relay.fds[1], WirePath::Direct);

        fiber::event::EventLoopGroup group(2);
        group.start();
        fiber::net::SocketAddress peer(fiber::net::IpAddress::loopback_v4(), 0);
        auto server_transport_result =
                fiber::http::TlsTransport::create(group.at(0), fiber::net::AcceptResult(server_fds[0], peer));
        auto client_transport_result =
                fiber::http::TlsTransport::create(group.at(1), fiber::net::AcceptResult(client_fds[0], peer));
        ASSERT_TRUE(server_transport_result);
        ASSERT_TRUE(client_transport_result);
        auto *server_transport = server_transport_result->release();
        auto *client_transport = client_transport_result->release();

        fiber::mem::IoBufChain chain;
        const std::vector<std::size_t> sizes = {9, 16384, 9, 16384, 9, 16384, 20000, 9, 300};
        const std::string expected = build_distinct_chain(pool, chain, sizes);

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
        relay.up.join(); // both streams closed: the record log is complete
        relay.down.join();

        ASSERT_TRUE(client_result);
        ASSERT_TRUE(server_result);
        EXPECT_TRUE(*server_result == expected);
        // 3 x (9 + 16384) = 49179 -> three full records + 27 bytes carried into
        // the fourth, which is filled from the 20000 node (one 64 KiB batch);
        // its 3643-byte tail is coalesced with the trailing [9][300].
        EXPECT_EQ(*client_result, (std::vector<std::size_t>{65536, 3643 + 9 + 300}));

        // Client records after ClientHello/CCS: [Finished][app data x5][close_notify].
        std::vector<std::size_t> sealed;
        for (const auto &[type, len]: records) {
            if (type == 23) {
                sealed.push_back(len - 17);
            }
        }
        ASSERT_GE(sealed.size(), 7u);
        const std::vector<std::size_t> app(sealed.begin() + 1, sealed.end() - 1);
        EXPECT_EQ(app, (std::vector<std::size_t>{16384, 16384, 16384, 16384, 3643 + 9 + 300}));
        EXPECT_EQ(sealed.back(), 2u); // close_notify: level + description
    });
}

} // namespace
