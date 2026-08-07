#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WaitGroup.h>
#include <fiber/async/WhenAny.h>
#include <fiber/async/Yield.h>
#include <fiber/common/IoError.h>
#include <fiber/common/mem/BufPool.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/ClientHttp1Exchange.h>
#include <fiber/http/Http1ConnectionGroupKey.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/StealableHttp1ConnectionPoolSet.h>
#include <fiber/net/TcpListener.h>
#include <fiber/net/TlsContext.h>
#include <fiber/net/detail/TlsStreamFd.h>

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;

class SuspendFalseAwaiter {
public:
    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<>) noexcept {
        completed_ = true;
        return false;
    }

    void await_resume() const noexcept { FIBER_ASSERT(completed_); }
    [[nodiscard]] bool completed() const noexcept { return completed_; }

private:
    bool completed_ = false;
};

struct BlockingLoopGate {
    std::promise<void> *entered = nullptr;
    std::shared_future<void> *release = nullptr;
    fiber::event::EventLoop::NotifyEntry notify{};

    static void run(BlockingLoopGate *gate) noexcept {
        FIBER_ASSERT(gate != nullptr);
        FIBER_ASSERT(gate->entered != nullptr);
        FIBER_ASSERT(gate->release != nullptr);
        gate->entered->set_value();
        gate->release->wait();
    }
};

using StealableAcquireAwaiter = fiber::http::StealableHttp1ConnectionPoolSet::AcquireAwaiter;
static_assert(fiber::async::SelectableAwaiter<StealableAcquireAwaiter>);

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
    std::string path = "/tmp/fiber_http1_pool_";
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

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) {
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }
    fiber::net::SocketAddress local;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), len, local)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return local.port();
}

fiber::http::Http1ClientConnectionOptions client_options(std::uint16_t port) {
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = false;
    return options;
}

fiber::http::Http1ClientConnectionOptions https_client_options(std::uint16_t port) {
    fiber::http::Http1ClientConnectionOptions options;
    options.peer_addr = fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), port);
    options.tls.enabled = true;
    options.tls.server_name = "localhost";
    return options;
}

struct HoldServerState {
    std::atomic_bool stop{false};
};

DetachedTask run_hold_server(fiber::event::EventLoop *loop, std::size_t accept_count,
                             std::promise<std::uint16_t> *port_promise,
                             std::promise<fiber::common::IoErr> *result_promise,
                             std::shared_ptr<HoldServerState> state) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        listener.close();
        result_promise->set_value(port_result.error());
        co_return;
    }

    std::vector<int> accepted_fds;
    accepted_fds.reserve(accept_count);
    for (std::size_t i = 0; i < accept_count; ++i) {
        auto accept_result = co_await listener.accept();
        if (!accept_result) {
            for (int fd: accepted_fds) {
                ::close(fd);
            }
            listener.close();
            result_promise->set_value(accept_result.error());
            co_return;
        }
        accepted_fds.push_back(accept_result->release_fd());
    }

    listener.close();
    while (!state->stop.load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(1ms);
    }

    for (int fd: accepted_fds) {
        ::close(fd);
    }
    result_promise->set_value(fiber::common::IoErr::None);
}

DetachedTask run_reset_after_accept_server(fiber::event::EventLoop *loop, std::promise<std::uint16_t> *port_promise,
                                           std::promise<fiber::common::IoErr> *result_promise) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        listener.close();
        result_promise->set_value(port_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    if (!accept_result) {
        listener.close();
        result_promise->set_value(accept_result.error());
        co_return;
    }

    int client = accept_result->release_fd();
    listener.close();
    linger reset_linger{1, 0};
    if (::setsockopt(client, SOL_SOCKET, SO_LINGER, &reset_linger, sizeof(reset_linger)) != 0) {
        const fiber::common::IoErr err = fiber::common::io_err_from_errno(errno);
        ::close(client);
        result_promise->set_value(err);
        co_return;
    }
    ::close(client);
    result_promise->set_value(fiber::common::IoErr::None);
}

DetachedTask run_tls_hold_server(fiber::event::EventLoop *loop, std::string cert_path, std::string key_path,
                                 std::promise<std::uint16_t> *port_promise,
                                 std::promise<fiber::common::IoErr> *result_promise,
                                 std::shared_ptr<HoldServerState> state) {
    fiber::net::TcpListener listener(*loop);
    fiber::net::ListenOptions options{};
    auto bind_result = listener.bind(fiber::net::SocketAddress(fiber::net::IpAddress::loopback_v4(), 0), options);
    if (!bind_result) {
        port_promise->set_value(0);
        result_promise->set_value(bind_result.error());
        co_return;
    }

    auto port_result = resolve_port(listener.fd());
    port_promise->set_value(port_result ? *port_result : 0);
    if (!port_result) {
        listener.close();
        result_promise->set_value(port_result.error());
        co_return;
    }

    fiber::net::TlsOptions server_options{};
    server_options.cert_file = std::move(cert_path);
    server_options.key_file = std::move(key_path);
    fiber::net::TlsContext server_ctx(std::move(server_options), true);
    auto init_result = server_ctx.init();
    if (!init_result) {
        listener.close();
        result_promise->set_value(init_result.error());
        co_return;
    }

    auto accept_result = co_await listener.accept();
    if (!accept_result) {
        listener.close();
        result_promise->set_value(accept_result.error());
        co_return;
    }

    fiber::net::detail::TlsStreamFd stream(*loop, accept_result->release_fd());
    auto stream_init_result = stream.init(server_ctx.raw(), true);
    if (!stream_init_result) {
        listener.close();
        result_promise->set_value(stream_init_result.error());
        co_return;
    }

    auto handshake_result = co_await stream.handshake();
    if (!handshake_result) {
        stream.close();
        listener.close();
        result_promise->set_value(handshake_result.error());
        co_return;
    }

    listener.close();
    while (!state->stop.load(std::memory_order_acquire)) {
        co_await fiber::async::sleep(1ms);
    }

    stream.close();
    result_promise->set_value(fiber::common::IoErr::None);
}

fiber::async::Task<fiber::common::IoResult<fiber::http::Http1ClientConnection *>>
ensure_connected(fiber::http::StealableHttp1ConnectionPoolSet::Lease &lease, std::uint16_t port) {
    if (!lease.valid()) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    if (!lease.has_connection()) {
        auto conn_result = lease.emplace_connection(client_options(port));
        if (!conn_result) {
            co_return std::unexpected(conn_result.error());
        }
        auto &conn = **conn_result;
        auto connect_result = co_await conn.connect(5s);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
    }
    co_return lease.get();
}

fiber::async::Task<fiber::common::IoResult<fiber::http::Http1ClientConnection *>>
ensure_connected(fiber::http::StealableHttp1ConnectionPoolSet::Lease &lease,
                 fiber::http::Http1ClientConnectionOptions options) {
    if (!lease.valid()) {
        co_return std::unexpected(fiber::common::IoErr::NoMem);
    }
    if (!lease.has_connection()) {
        auto conn_result = lease.emplace_connection(std::move(options));
        if (!conn_result) {
            co_return std::unexpected(conn_result.error());
        }
        auto &conn = **conn_result;
        auto connect_result = co_await conn.connect(5s);
        if (!connect_result) {
            co_return std::unexpected(connect_result.error());
        }
    }
    co_return lease.get();
}

fiber::async::Task<fiber::http::StealableHttp1ConnectionPoolSet::Lease>
acquire_in_task(fiber::http::StealableHttp1ConnectionPoolSet *set, const fiber::http::Http1ConnectionGroupKey *key) {
    co_return co_await set->acquire(*key);
}

DetachedTask run_https_home_connect(fiber::http::StealableHttp1ConnectionPoolSet *set,
                                    const fiber::http::Http1ConnectionGroupKey *key, std::uint16_t port,
                                    std::atomic<fiber::http::Http1ClientConnection *> *home_conn,
                                    std::atomic_bool *done) {
    auto lease = co_await set->acquire(*key);
    auto conn_result = co_await ensure_connected(lease, https_client_options(port));
    home_conn->store(conn_result ? *conn_result : nullptr, std::memory_order_release);
    done->store(true, std::memory_order_release);
    lease.reset();
    co_return;
}

DetachedTask run_https_borrow_and_release(fiber::http::StealableHttp1ConnectionPoolSet *set,
                                          const fiber::http::Http1ConnectionGroupKey *key,
                                          fiber::http::Http1ClientConnection *expected_conn,
                                          fiber::event::EventLoop *expected_home_loop, std::atomic_bool *ok,
                                          std::atomic_bool *done) {
    auto borrowed = co_await set->acquire(*key);
    ok->store(borrowed.valid() && borrowed.hit() && borrowed.has_connection() && borrowed.get() == expected_conn &&
                      &borrowed.connection().loop() == expected_home_loop,
              std::memory_order_release);
    borrowed.reset();
    done->store(true, std::memory_order_release);
    co_return;
}

DetachedTask run_https_reacquire_and_close(fiber::http::StealableHttp1ConnectionPoolSet *set,
                                           const fiber::http::Http1ConnectionGroupKey *key,
                                           fiber::http::Http1ClientConnection *expected_conn, std::atomic_bool *ok,
                                           std::atomic_bool *done) {
    co_await fiber::async::sleep(10ms);
    auto returned = co_await set->acquire(*key);
    const bool returned_ok =
            returned.valid() && returned.hit() && returned.has_connection() && returned.get() == expected_conn;
    if (returned_ok) {
        returned.connection().close();
    }
    returned.reset();
    ok->store(returned_ok, std::memory_order_release);
    done->store(true, std::memory_order_release);
    co_return;
}

struct AbortBlockedReadOutcome {
    bool borrowed_hit = false;
    bool reusable_after_abort = true;
    fiber::common::IoErr send_error = fiber::common::IoErr::Unknown;
    fiber::common::IoErr abort_error = fiber::common::IoErr::Unknown;
    fiber::common::IoErr read_error = fiber::common::IoErr::Unknown;
    fiber::common::IoErr write_error = fiber::common::IoErr::Unknown;
};

struct AbandonedReadOutcome {
    bool borrowed_hit = false;
    bool timeout_won = false;
    bool exchange_valid_after_cancel = true;
    bool reusable_after_cancel = true;
    fiber::common::IoErr send_error = fiber::common::IoErr::Unknown;
};

DetachedTask run_blocked_header_read(fiber::http::ClientHttp1Exchange *exchange, fiber::async::WaitGroup *done,
                                     AbortBlockedReadOutcome *outcome) {
    auto result = co_await exchange->read_header();
    outcome->read_error = result ? fiber::common::IoErr::None : result.error();
    done->done();
    co_return;
}

DetachedTask run_blocked_body_write(fiber::http::ClientHttp1Exchange *exchange, const std::vector<std::uint8_t> *body,
                                    fiber::async::WaitGroup *done, AbortBlockedReadOutcome *outcome) {
    auto result = co_await exchange->write_all(body->data(), body->size(), true);
    outcome->write_error = result ? fiber::common::IoErr::None : result.error();
    done->done();
    co_return;
}

TEST(StealableHttp1ConnectionPoolSetTest, StealsIdleConnectionFromOtherLoopAndReturnsItHome) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet::Options options{};
    options.max_idle_per_group = 2;
    options.max_idle_total = 4;
    options.initial_group_capacity = 2;
    fiber::http::StealableHttp1ConnectionPoolSet set(group, options);
    ASSERT_TRUE(set.init());

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<fiber::http::Http1ClientConnection *> home_conn_promise;
    auto home_conn_future = home_conn_promise.get_future();
    std::promise<bool> final_promise;
    auto final_future = final_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        if (!conn_result) {
            home_conn_promise.set_value(nullptr);
            co_return;
        }
        home_conn_promise.set_value(*conn_result);
        lease.reset();
    });

    auto *home_conn = home_conn_future.get();
    ASSERT_NE(home_conn, nullptr);

    fiber::async::spawn(group.at(1), [&, home_conn]() -> DetachedTask {
        auto borrowed = co_await set.acquire(key);
        const bool borrowed_ok = borrowed.valid() && borrowed.hit() && borrowed.has_connection() &&
                                 borrowed.get() == home_conn && &borrowed.connection().loop() == &group.at(0);
        borrowed.reset();

        fiber::async::spawn(group.at(0), [&, borrowed_ok, home_conn]() -> DetachedTask {
            auto returned = co_await set.acquire(key);
            const bool returned_ok =
                    returned.valid() && returned.hit() && returned.has_connection() && returned.get() == home_conn;
            if (returned_ok) {
                returned.connection().close();
            }
            returned.reset();
            final_promise.set_value(borrowed_ok && returned_ok);
        });
    });

    EXPECT_TRUE(final_future.get());

    server_state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, WhenAnyCancellationKeepsRemoteAcquireStateAliveUntilDrained) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    auto *home_loop = &group.at(0);
    auto *borrower_loop = &group.at(1);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<fiber::http::Http1ClientConnection *> home_ready_promise;
    auto home_ready_future = home_ready_promise.get_future();

    group.start();
    fiber::async::spawn(*home_loop, [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        home_ready_promise.set_value(conn_result ? *conn_result : nullptr);
        lease.reset();
    });

    auto *home_conn = home_ready_future.get();
    ASSERT_NE(home_conn, nullptr);

    std::promise<void> direct_gate_entered_promise;
    auto direct_gate_entered = direct_gate_entered_promise.get_future();
    std::promise<void> direct_gate_release_promise;
    auto direct_gate_release = direct_gate_release_promise.get_future().share();
    BlockingLoopGate direct_gate{&direct_gate_entered_promise, &direct_gate_release};
    home_loop->post<BlockingLoopGate, &BlockingLoopGate::notify, &BlockingLoopGate::run>(direct_gate);
    ASSERT_EQ(direct_gate_entered.wait_for(2s), std::future_status::ready);

    std::promise<bool> direct_cancel_promise;
    auto direct_cancel_future = direct_cancel_promise.get_future();
    fiber::async::spawn(*borrower_loop, [&]() -> DetachedTask {
        auto result = co_await fiber::async::when_any([&]() { return set.acquire(key); },
                                                      []() { return SuspendFalseAwaiter{}; });
        direct_cancel_promise.set_value(result.is<1>());
    });
    ASSERT_EQ(direct_cancel_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(direct_cancel_future.get());
    direct_gate_release_promise.set_value();

    std::promise<bool> direct_reacquire_promise;
    auto direct_reacquire_future = direct_reacquire_promise.get_future();
    fiber::async::spawn(*home_loop, [&, home_conn]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        direct_reacquire_promise.set_value(lease.valid() && lease.hit() && lease.has_connection() &&
                                           lease.get() == home_conn);
        lease.reset();
    });
    ASSERT_EQ(direct_reacquire_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(direct_reacquire_future.get());

    std::promise<void> task_gate_entered_promise;
    auto task_gate_entered = task_gate_entered_promise.get_future();
    std::promise<void> task_gate_release_promise;
    auto task_gate_release = task_gate_release_promise.get_future().share();
    BlockingLoopGate task_gate{&task_gate_entered_promise, &task_gate_release};
    home_loop->post<BlockingLoopGate, &BlockingLoopGate::notify, &BlockingLoopGate::run>(task_gate);
    ASSERT_EQ(task_gate_entered.wait_for(2s), std::future_status::ready);

    std::promise<bool> task_cancel_promise;
    auto task_cancel_future = task_cancel_promise.get_future();
    fiber::async::spawn(*borrower_loop, [&]() -> DetachedTask {
        auto result = co_await fiber::async::when_any([&]() { return acquire_in_task(&set, &key).select(); },
                                                      []() { return fiber::async::yield(); });
        task_cancel_promise.set_value(result.is<1>());
    });
    ASSERT_EQ(task_cancel_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(task_cancel_future.get());
    task_gate_release_promise.set_value();

    std::promise<bool> task_reacquire_promise;
    auto task_reacquire_future = task_reacquire_promise.get_future();
    fiber::async::spawn(*home_loop, [&, home_conn]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        task_reacquire_promise.set_value(lease.valid() && lease.hit() && lease.has_connection() &&
                                         lease.get() == home_conn);
        lease.reset();
    });
    ASSERT_EQ(task_reacquire_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(task_reacquire_future.get());

    std::promise<void> hit_home_gate_entered_promise;
    auto hit_home_gate_entered = hit_home_gate_entered_promise.get_future();
    std::promise<void> hit_home_gate_release_promise;
    auto hit_home_gate_release = hit_home_gate_release_promise.get_future().share();
    BlockingLoopGate hit_home_gate{&hit_home_gate_entered_promise, &hit_home_gate_release};
    home_loop->post<BlockingLoopGate, &BlockingLoopGate::notify, &BlockingLoopGate::run>(hit_home_gate);
    ASSERT_EQ(hit_home_gate_entered.wait_for(2s), std::future_status::ready);

    fiber::async::WaitGroup hit_winner;
    hit_winner.add();
    std::promise<bool> hit_cancel_promise;
    auto hit_cancel_future = hit_cancel_promise.get_future();
    fiber::async::spawn(*borrower_loop, [&]() -> DetachedTask {
        auto result = co_await fiber::async::when_any([&]() { return set.acquire(key); },
                                                      [&]() { return hit_winner.join(); });
        hit_cancel_promise.set_value(result.is<1>());
    });

    std::promise<void> hit_borrower_gate_entered_promise;
    auto hit_borrower_gate_entered = hit_borrower_gate_entered_promise.get_future();
    std::promise<void> hit_borrower_gate_release_promise;
    auto hit_borrower_gate_release = hit_borrower_gate_release_promise.get_future().share();
    BlockingLoopGate hit_borrower_gate{&hit_borrower_gate_entered_promise, &hit_borrower_gate_release};
    borrower_loop->post<BlockingLoopGate, &BlockingLoopGate::notify, &BlockingLoopGate::run>(hit_borrower_gate);
    ASSERT_EQ(hit_borrower_gate_entered.wait_for(2s), std::future_status::ready);

    hit_winner.done();
    hit_home_gate_release_promise.set_value();

    std::promise<void> hit_processed_promise;
    auto hit_processed_future = hit_processed_promise.get_future();
    fiber::async::spawn(*home_loop, [&]() -> DetachedTask {
        hit_processed_promise.set_value();
        co_return;
    });
    ASSERT_EQ(hit_processed_future.wait_for(2s), std::future_status::ready);

    hit_borrower_gate_release_promise.set_value();
    ASSERT_EQ(hit_cancel_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(hit_cancel_future.get());

    std::promise<void> hit_cancel_drained_promise;
    auto hit_cancel_drained_future = hit_cancel_drained_promise.get_future();
    fiber::async::spawn(*borrower_loop, [&]() -> DetachedTask {
        hit_cancel_drained_promise.set_value();
        co_return;
    });
    ASSERT_EQ(hit_cancel_drained_future.wait_for(2s), std::future_status::ready);

    std::promise<bool> hit_reacquire_promise;
    auto hit_reacquire_future = hit_reacquire_promise.get_future();
    fiber::async::spawn(*home_loop, [&, home_conn]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        hit_reacquire_promise.set_value(lease.valid() && lease.hit() && lease.has_connection() &&
                                        lease.get() == home_conn);
        lease.reset();
    });
    ASSERT_EQ(hit_reacquire_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(hit_reacquire_future.get());

    std::promise<void> shutdown_gate_entered_promise;
    auto shutdown_gate_entered = shutdown_gate_entered_promise.get_future();
    std::promise<void> shutdown_gate_release_promise;
    auto shutdown_gate_release = shutdown_gate_release_promise.get_future().share();
    BlockingLoopGate shutdown_gate{&shutdown_gate_entered_promise, &shutdown_gate_release};
    home_loop->post<BlockingLoopGate, &BlockingLoopGate::notify, &BlockingLoopGate::run>(shutdown_gate);
    ASSERT_EQ(shutdown_gate_entered.wait_for(2s), std::future_status::ready);

    std::promise<bool> shutdown_cancel_promise;
    auto shutdown_cancel_future = shutdown_cancel_promise.get_future();
    fiber::async::spawn(*borrower_loop, [&]() -> DetachedTask {
        auto result = co_await fiber::async::when_any([&]() { return set.acquire(key); },
                                                      []() { return SuspendFalseAwaiter{}; });
        shutdown_cancel_promise.set_value(result.is<1>());
    });
    ASSERT_EQ(shutdown_cancel_future.wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(shutdown_cancel_future.get());

    std::promise<void> shutdown_done_promise;
    auto shutdown_done_future = shutdown_done_promise.get_future();
    fiber::async::spawn(*borrower_loop, [&]() -> DetachedTask {
        co_await set.shutdown_async();
        shutdown_done_promise.set_value();
    });
    EXPECT_EQ(shutdown_done_future.wait_for(20ms), std::future_status::timeout);
    shutdown_gate_release_promise.set_value();
    ASSERT_EQ(shutdown_done_future.wait_for(2s), std::future_status::ready);

    server_state->stop.store(true, std::memory_order_release);
    ASSERT_EQ(server_result_future.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);

    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, LocalMissReturnsCallerLoopLeaseWhenNoRemoteHintMatches) {
    fiber::event::EventLoopGroup group(2);
    auto *loop0 = &group.at(0);
    auto *loop1 = &group.at(1);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());

    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), 80,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<bool> result_promise;
    auto result_future = result_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = lease.emplace_connection({});
        const bool ok = lease.valid() && !lease.hit() && conn_result.has_value() && lease.get() != nullptr &&
                        &lease.connection().loop() == &fiber::event::EventLoop::current();
        if (ok) {
            lease.connection().close();
        }
        lease.reset();
        result_promise.set_value(ok);
    });

    EXPECT_TRUE(result_future.get());
    group.stop();
    group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, ClearAllowsBorrowedConnectionToReturnHome) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    auto *loop0 = &group.at(0);
    auto *loop1 = &group.at(1);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<fiber::http::Http1ClientConnection *> home_conn_promise;
    auto home_conn_future = home_conn_promise.get_future();
    std::promise<void> borrowed_ready_promise;
    auto borrowed_ready_future = borrowed_ready_promise.get_future();
    auto allow_reset = std::make_shared<std::atomic_bool>(false);
    std::promise<bool> final_promise;
    auto final_future = final_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        home_conn_promise.set_value(conn_result ? *conn_result : nullptr);
        lease.reset();
    });

    auto *home_conn = home_conn_future.get();
    ASSERT_NE(home_conn, nullptr);

    fiber::async::spawn(group.at(1), [&, home_conn]() -> DetachedTask {
        auto borrowed = co_await set.acquire(key);
        const bool borrowed_ok =
                borrowed.valid() && borrowed.hit() && borrowed.has_connection() && borrowed.get() == home_conn;
        borrowed_ready_promise.set_value();
        while (!allow_reset->load(std::memory_order_acquire)) {
            co_await fiber::async::sleep(1ms);
        }
        borrowed.reset();
        fiber::async::spawn(group.at(0), [&, borrowed_ok, home_conn]() -> DetachedTask {
            co_await fiber::async::sleep(10ms);
            auto returned = co_await set.acquire(key);
            const bool returned_ok =
                    returned.valid() && returned.hit() && returned.has_connection() && returned.get() == home_conn;
            if (returned_ok) {
                returned.connection().close();
            }
            returned.reset();
            final_promise.set_value(borrowed_ok && returned_ok);
        });
    });

    borrowed_ready_future.get();
    std::promise<void> clear_done_promise;
    auto clear_done_future = clear_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await set.clear_async();
        clear_done_promise.set_value();
    });
    clear_done_future.get();
    allow_reset->store(true, std::memory_order_release);
    EXPECT_TRUE(final_future.get());

    server_state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, ShutdownDropsBorrowedConnectionOnReturn) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    auto *loop0 = &group.at(0);
    auto *loop1 = &group.at(1);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<void> home_ready_promise;
    auto home_ready_future = home_ready_promise.get_future();
    std::promise<void> borrowed_ready_promise;
    auto borrowed_ready_future = borrowed_ready_promise.get_future();
    auto allow_reset = std::make_shared<std::atomic_bool>(false);
    std::promise<bool> final_promise;
    auto final_future = final_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        if (conn_result) {
            home_ready_promise.set_value();
            lease.reset();
        } else {
            home_ready_promise.set_value();
        }
    });

    home_ready_future.get();

    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        auto borrowed = co_await set.acquire(key);
        borrowed_ready_promise.set_value();
        while (!allow_reset->load(std::memory_order_acquire)) {
            co_await fiber::async::sleep(1ms);
        }
        borrowed.reset();
        fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
            co_await fiber::async::sleep(10ms);
            auto after_shutdown = co_await set.acquire(key);
            final_promise.set_value(!after_shutdown.valid());
        });
    });

    borrowed_ready_future.get();
    std::promise<void> shutdown_done_promise;
    auto shutdown_done_future = shutdown_done_promise.get_future();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await set.shutdown_async();
        shutdown_done_promise.set_value();
    });
    shutdown_done_future.get();
    allow_reset->store(true, std::memory_order_release);
    EXPECT_TRUE(final_future.get());

    server_state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, BorrowedConnectionHeldByOneLoopMakesOtherLoopMiss) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(3);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<void> home_ready_promise;
    auto home_ready_future = home_ready_promise.get_future();
    std::promise<void> borrower_holds_promise;
    auto borrower_holds_future = borrower_holds_promise.get_future();
    auto allow_release = std::make_shared<std::atomic_bool>(false);
    std::promise<bool> miss_promise;
    auto miss_future = miss_promise.get_future();
    std::promise<void> cleanup_promise;
    auto cleanup_future = cleanup_promise.get_future();
    std::promise<void> final_close_promise;
    auto final_close_future = final_close_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        if (conn_result) {
            lease.reset();
        }
        home_ready_promise.set_value();
        co_return;
    });

    home_ready_future.get();

    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        auto borrowed = co_await set.acquire(key);
        borrower_holds_promise.set_value();
        while (!allow_release->load(std::memory_order_acquire)) {
            co_await fiber::async::sleep(1ms);
        }
        borrowed.reset();
        cleanup_promise.set_value();
        co_return;
    });

    borrower_holds_future.get();

    fiber::async::spawn(group.at(2), [&]() -> DetachedTask {
        auto miss = co_await set.acquire(key);
        miss_promise.set_value(miss.valid() && !miss.hit() && !miss.has_connection());
        co_return;
    });

    EXPECT_TRUE(miss_future.get());
    allow_release->store(true, std::memory_order_release);
    cleanup_future.get();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto returned = co_await set.acquire(key);
        if (returned.hit() && returned.has_connection()) {
            returned.connection().close();
        }
        returned.reset();
        final_close_promise.set_value();
        co_return;
    });
    final_close_future.get();

    server_state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, BorrowedConnectionFailureOnBorrowerLoopDropsItAtHome) {
    fiber::event::EventLoopGroup server_group(1);
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_reset_after_accept_server(&server_group.at(0), &server_port_promise, &server_result_promise);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<void> home_ready_promise;
    auto home_ready_future = home_ready_promise.get_future();
    std::promise<bool> borrower_failed_promise;
    auto borrower_failed_future = borrower_failed_promise.get_future();
    std::promise<bool> dropped_promise;
    auto dropped_future = dropped_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        if (conn_result) {
            lease.reset();
        }
        home_ready_promise.set_value();
        co_return;
    });

    home_ready_future.get();

    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        auto borrowed = co_await set.acquire(key);
        bool failed = false;
        if (borrowed.hit() && borrowed.has_connection()) {
            fiber::mem::BufPool pool;
            fiber::http::HttpHeaders headers(pool);
            headers.add_view("host", "example.com");
            fiber::http::ClientHttp1Exchange exchange(borrowed.connection(), pool);
            fiber::http::Http1RequestHead head;
            head.method = fiber::http::HttpMethod::Get;
            head.target = "/drop";
            head.headers = &headers;

            auto send_result = co_await exchange.send_header(head, true);
            if (!send_result) {
                failed = true;
            } else {
                auto header_result = co_await exchange.read_header();
                failed = !header_result.has_value();
            }
        }
        borrowed.reset();
        borrower_failed_promise.set_value(failed);
        co_return;
    });

    EXPECT_TRUE(borrower_failed_future.get());

    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await fiber::async::sleep(10ms);
        auto after_close = co_await set.acquire(key);
        dropped_promise.set_value(after_close.valid() && !after_close.hit() && !after_close.has_connection());
        co_return;
    });

    EXPECT_TRUE(dropped_future.get());
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, AbortBlockedReadAndWriteBeforeReturningStolenConnection) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<bool> home_ready_promise;
    std::promise<AbortBlockedReadOutcome> borrower_promise;
    std::promise<bool> dropped_promise;
    auto home_ready_future = home_ready_promise.get_future();
    auto borrower_future = borrower_promise.get_future();
    auto dropped_future = dropped_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        const bool connected = conn_result.has_value();
        lease.reset();
        home_ready_promise.set_value(connected);
        co_return;
    });

    ASSERT_TRUE(home_ready_future.get());

    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        AbortBlockedReadOutcome outcome;
        auto borrowed = co_await set.acquire(key);
        outcome.borrowed_hit = borrowed.valid() && borrowed.hit() && borrowed.has_connection();
        if (outcome.borrowed_hit) {
            fiber::mem::BufPool pool;
            fiber::http::HttpHeaders headers(pool);
            headers.add_view("host", "example.com");
            std::vector<std::uint8_t> body(32U * 1024U * 1024U, static_cast<std::uint8_t>('x'));
            fiber::http::ClientHttp1Exchange exchange(borrowed.connection(), pool);
            fiber::http::Http1RequestHead head;
            head.method = fiber::http::HttpMethod::Post;
            head.target = "/blocked";
            head.headers = &headers;
            head.body = fiber::http::HttpBodySpec::ContentLength(body.size());

            auto send_result = co_await exchange.send_header(head, false);
            outcome.send_error = send_result ? fiber::common::IoErr::None : send_result.error();
            if (send_result) {
                fiber::async::WaitGroup io_done;
                io_done.add(2);
                fiber::async::spawn([&]() { return run_blocked_header_read(&exchange, &io_done, &outcome); });
                fiber::async::spawn([&]() { return run_blocked_body_write(&exchange, &body, &io_done, &outcome); });
                co_await fiber::async::sleep(10ms);

                auto abort_result = exchange.abort(fiber::common::IoErr::ConnAborted);
                outcome.abort_error = abort_result ? fiber::common::IoErr::None : abort_result.error();
                outcome.reusable_after_abort = borrowed.connection().reusable();
                co_await io_done.join();
            }
        }
        borrowed.reset();
        borrower_promise.set_value(outcome);
        co_return;
    });

    ASSERT_EQ(borrower_future.wait_for(10s), std::future_status::ready);
    const auto outcome = borrower_future.get();
    EXPECT_TRUE(outcome.borrowed_hit);
    EXPECT_EQ(outcome.send_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.abort_error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.read_error, fiber::common::IoErr::ConnAborted);
    EXPECT_EQ(outcome.write_error, fiber::common::IoErr::ConnAborted);
    EXPECT_FALSE(outcome.reusable_after_abort);

    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await fiber::async::sleep(10ms);
        auto after_abort = co_await set.acquire(key);
        dropped_promise.set_value(after_abort.valid() && !after_abort.hit() && !after_abort.has_connection());
        co_return;
    });

    ASSERT_EQ(dropped_future.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(dropped_future.get());

    server_state->stop.store(true, std::memory_order_release);
    ASSERT_EQ(server_result_future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, AbandonedReadTaskInvalidatesExchangeBeforeReturningStolenConnection) {
    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_hold_server(&server_group.at(0), 1, &server_port_promise, &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    const auto key = fiber::http::Http1ConnectionGroupKey::from_ip(fiber::net::IpAddress::loopback_v4(), port,
                                                                   fiber::http::Http1ConnectionGroupKey::Scheme::Http);

    std::promise<bool> home_ready_promise;
    std::promise<AbandonedReadOutcome> borrower_promise;
    std::promise<bool> dropped_promise;
    auto home_ready_future = home_ready_promise.get_future();
    auto borrower_future = borrower_promise.get_future();
    auto dropped_future = dropped_promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto lease = co_await set.acquire(key);
        auto conn_result = co_await ensure_connected(lease, port);
        const bool connected = conn_result.has_value();
        lease.reset();
        home_ready_promise.set_value(connected);
        co_return;
    });

    ASSERT_TRUE(home_ready_future.get());

    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        AbandonedReadOutcome outcome;
        auto borrowed = co_await set.acquire(key);
        outcome.borrowed_hit = borrowed.valid() && borrowed.hit() && borrowed.has_connection();
        if (outcome.borrowed_hit) {
            {
                fiber::mem::BufPool pool;
                fiber::http::HttpHeaders headers(pool);
                headers.add_view("host", "example.com");
                fiber::http::ClientHttp1Exchange exchange(borrowed.connection(), pool);
                fiber::http::Http1RequestHead head;
                head.method = fiber::http::HttpMethod::Get;
                head.target = "/abandoned-read";
                head.headers = &headers;

                auto send_result = co_await exchange.send_header(head, true);
                outcome.send_error = send_result ? fiber::common::IoErr::None : send_result.error();
                if (send_result) {
                    auto completed = co_await fiber::async::when_any([&]() { return exchange.read_header().select(); },
                                                                     []() { return fiber::async::sleep(10ms); });
                    outcome.timeout_won = completed.is<1>();
                    outcome.exchange_valid_after_cancel = exchange.valid();
                    outcome.reusable_after_cancel = borrowed.connection().reusable();
                }
            }
            borrowed.reset();
        }
        borrower_promise.set_value(outcome);
        co_return;
    });

    ASSERT_EQ(borrower_future.wait_for(10s), std::future_status::ready);
    const auto outcome = borrower_future.get();
    EXPECT_TRUE(outcome.borrowed_hit);
    EXPECT_EQ(outcome.send_error, fiber::common::IoErr::None);
    EXPECT_TRUE(outcome.timeout_won);
    EXPECT_FALSE(outcome.exchange_valid_after_cancel);
    EXPECT_FALSE(outcome.reusable_after_cancel);

    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        co_await fiber::async::sleep(10ms);
        auto after_cancel = co_await set.acquire(key);
        dropped_promise.set_value(after_cancel.valid() && !after_cancel.hit() && !after_cancel.has_connection());
        co_return;
    });

    ASSERT_EQ(dropped_future.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(dropped_future.get());

    server_state->stop.store(true, std::memory_order_release);
    ASSERT_EQ(server_result_future.wait_for(10s), std::future_status::ready);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

TEST(StealableHttp1ConnectionPoolSetTest, HttpsConnectionCanBeStolenAndReturnedHome) {
    SigpipeGuard sigpipe_guard;
    TempFile cert("cert", kSelfSignedCertPem);
    TempFile key_file("key", kSelfSignedKeyPem);
    ASSERT_TRUE(cert.ok);
    ASSERT_TRUE(key_file.ok);

    fiber::event::EventLoopGroup server_group(1);
    auto server_state = std::make_shared<HoldServerState>();
    std::promise<std::uint16_t> server_port_promise;
    std::promise<fiber::common::IoErr> server_result_promise;
    auto server_port_future = server_port_promise.get_future();
    auto server_result_future = server_result_promise.get_future();

    server_group.start();
    fiber::async::spawn(server_group.at(0), [&]() {
        return run_tls_hold_server(&server_group.at(0), cert.path, key_file.path, &server_port_promise,
                                   &server_result_promise, server_state);
    });

    const std::uint16_t port = server_port_future.get();
    ASSERT_NE(port, 0);

    fiber::event::EventLoopGroup group(2);
    auto *loop0 = &group.at(0);
    auto *loop1 = &group.at(1);
    fiber::http::StealableHttp1ConnectionPoolSet set(group);
    ASSERT_TRUE(set.init());
    auto key_result = fiber::http::Http1ConnectionGroupKey::from_name(
            "localhost", port, fiber::http::Http1ConnectionGroupKey::Scheme::Https);
    ASSERT_TRUE(key_result.has_value());
    const auto key = *key_result;

    auto home_conn = std::make_shared<std::atomic<fiber::http::Http1ClientConnection *>>(nullptr);
    auto home_ready = std::make_shared<std::atomic_bool>(false);
    auto borrowed_ok = std::make_shared<std::atomic_bool>(false);
    auto borrowed_done = std::make_shared<std::atomic_bool>(false);
    auto returned_ok = std::make_shared<std::atomic_bool>(false);
    auto returned_done = std::make_shared<std::atomic_bool>(false);

    group.start();
    fiber::async::spawn(*loop0,
                        [&]() { return run_https_home_connect(&set, &key, port, home_conn.get(), home_ready.get()); });

    for (int i = 0; i < 2000 && !home_ready->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(home_ready->load(std::memory_order_acquire));
    auto *home_conn_ptr = home_conn->load(std::memory_order_acquire);
    ASSERT_NE(home_conn_ptr, nullptr);

    fiber::async::spawn(*loop1, [&]() {
        return run_https_borrow_and_release(&set, &key, home_conn_ptr, loop0, borrowed_ok.get(), borrowed_done.get());
    });

    for (int i = 0; i < 2000 && !borrowed_done->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(borrowed_done->load(std::memory_order_acquire));
    ASSERT_TRUE(borrowed_ok->load(std::memory_order_acquire));

    fiber::async::spawn(*loop0, [&]() {
        return run_https_reacquire_and_close(&set, &key, home_conn_ptr, returned_ok.get(), returned_done.get());
    });

    for (int i = 0; i < 2000 && !returned_done->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(returned_done->load(std::memory_order_acquire));
    EXPECT_TRUE(returned_ok->load(std::memory_order_acquire));

    server_state->stop.store(true, std::memory_order_release);
    EXPECT_EQ(server_result_future.get(), fiber::common::IoErr::None);
    group.stop();
    group.join();
    server_group.stop();
    server_group.join();
}

} // namespace
