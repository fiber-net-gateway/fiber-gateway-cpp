#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <async/Spawn.h>
#include <async/Timeout.h>
#include <event/EventLoopGroup.h>
#include <fiber/nacos/NacosClient.h>

namespace {

using namespace std::chrono_literals;
using fiber::async::DetachedTask;
using fiber::nacos::NacosAuthErrorCode;
using fiber::nacos::NacosAuthSnapshot;
using fiber::nacos::NacosAuthState;
using fiber::nacos::NacosClient;

struct ServerStep {
    int status = 200;
    std::string body;
    bool hold_until_peer_close = false;
};

class ScriptedHttpServer {
public:
    explicit ScriptedHttpServer(std::vector<ServerStep> steps) : steps_(std::move(steps)) {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return;
        }
        int reuse = 1;
        (void) ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            ::listen(fd, SOMAXCONN) != 0) {
            ::close(fd);
            return;
        }

        socklen_t length = sizeof(address);
        if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
            ::close(fd);
            return;
        }
        port_ = ntohs(address.sin_port);
        listen_fd_.store(fd, std::memory_order_release);
        thread_ = std::thread([this]() { run(); });
    }

    ScriptedHttpServer(const ScriptedHttpServer &) = delete;
    ScriptedHttpServer &operator=(const ScriptedHttpServer &) = delete;

    ~ScriptedHttpServer() {
        stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] bool valid() const noexcept { return port_ != 0 && listen_fd_.load(std::memory_order_acquire) >= 0; }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    [[nodiscard]] bool wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, timeout, [&]() { return requests_.size() >= count || finished_; }) &&
               requests_.size() >= count;
    }

    [[nodiscard]] bool wait_for_peer_close(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, timeout, [&]() { return peer_closed_ || finished_; }) && peer_closed_;
    }

    [[nodiscard]] std::vector<std::string> requests() const {
        std::lock_guard lock(mu_);
        return requests_;
    }

private:
    static bool send_all(int fd, std::string_view bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        return true;
    }

    static std::size_t parse_content_length(std::string_view header) {
        constexpr std::string_view Prefix = "Content-Length:";
        const std::size_t position = header.find(Prefix);
        if (position == std::string_view::npos) {
            return 0;
        }
        std::size_t cursor = position + Prefix.size();
        while (cursor < header.size() && header[cursor] == ' ') {
            ++cursor;
        }
        std::size_t value = 0;
        while (cursor < header.size() && header[cursor] >= '0' && header[cursor] <= '9') {
            value = value * 10 + static_cast<std::size_t>(header[cursor] - '0');
            ++cursor;
        }
        return value;
    }

    static bool read_request(int fd, std::string &request) {
        std::size_t required = 0;
        bool header_complete = false;
        for (;;) {
            if (header_complete && request.size() >= required) {
                return true;
            }
            char buffer[1024];
            const ssize_t read = ::recv(fd, buffer, sizeof(buffer), 0);
            if (read > 0) {
                request.append(buffer, static_cast<std::size_t>(read));
                if (!header_complete) {
                    const std::size_t header_end = request.find("\r\n\r\n");
                    if (header_end != std::string::npos) {
                        const std::size_t body_size =
                                parse_content_length(std::string_view(request).substr(0, header_end + 4));
                        required = header_end + 4 + body_size;
                        header_complete = true;
                    }
                }
                continue;
            }
            if (read < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
    }

    static std::string make_response(const ServerStep &step) {
        std::string response = "HTTP/1.1 ";
        response.append(std::to_string(step.status));
        response.append(step.status == 200 ? " OK\r\n" : " Error\r\n");
        response.append("Content-Type: application/json\r\nContent-Length: ");
        response.append(std::to_string(step.body.size()));
        response.append("\r\nConnection: close\r\n\r\n");
        response.append(step.body);
        return response;
    }

    void stop() noexcept {
        if (stopping_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const int active = active_fd_.load(std::memory_order_acquire);
        if (active >= 0) {
            (void) ::shutdown(active, SHUT_RDWR);
        }
        const int listener = listen_fd_.exchange(-1, std::memory_order_acq_rel);
        if (listener >= 0) {
            (void) ::shutdown(listener, SHUT_RDWR);
            ::close(listener);
        }
    }

    void run() {
        for (const ServerStep &step: steps_) {
            const int listener = listen_fd_.load(std::memory_order_acquire);
            if (listener < 0 || stopping_.load(std::memory_order_acquire)) {
                break;
            }

            int client = -1;
            for (;;) {
                client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
                if (client >= 0 || stopping_.load(std::memory_order_acquire)) {
                    break;
                }
                if (errno != EINTR) {
                    break;
                }
            }
            if (client < 0) {
                break;
            }
            active_fd_.store(client, std::memory_order_release);
            if (stopping_.load(std::memory_order_acquire)) {
                ::close(client);
                active_fd_.store(-1, std::memory_order_release);
                break;
            }

            std::string request;
            const bool request_ok = read_request(client, request);
            if (request_ok) {
                {
                    std::lock_guard lock(mu_);
                    requests_.push_back(std::move(request));
                }
                cv_.notify_all();
            }

            if (request_ok && step.hold_until_peer_close) {
                char byte = 0;
                for (;;) {
                    const ssize_t read = ::recv(client, &byte, sizeof(byte), 0);
                    if (read == 0) {
                        std::lock_guard lock(mu_);
                        peer_closed_ = true;
                        break;
                    }
                    if (read < 0 && errno == EINTR) {
                        continue;
                    }
                    if (read < 0 && stopping_.load(std::memory_order_acquire)) {
                        break;
                    }
                    if (read < 0) {
                        std::lock_guard lock(mu_);
                        peer_closed_ = true;
                        break;
                    }
                }
                cv_.notify_all();
            } else if (request_ok) {
                (void) send_all(client, make_response(step));
            }

            (void) ::shutdown(client, SHUT_RDWR);
            ::close(client);
            active_fd_.store(-1, std::memory_order_release);
        }

        {
            std::lock_guard lock(mu_);
            finished_ = true;
        }
        cv_.notify_all();
    }

    std::vector<ServerStep> steps_;
    std::atomic<int> listen_fd_{-1};
    std::atomic<int> active_fd_{-1};
    std::atomic<bool> stopping_{false};
    std::uint16_t port_ = 0;
    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::string> requests_;
    bool peer_closed_ = false;
    bool finished_ = false;
};

fiber::net::IpAddress parse_ip(std::string_view text) {
    fiber::net::IpAddress ip;
    EXPECT_TRUE(fiber::net::IpAddress::parse(text, ip));
    return ip;
}

fiber::nacos::NacosClientConfig
make_config(std::uint16_t port, fiber::nacos::NacosAuthApiVersion version = fiber::nacos::NacosAuthApiVersion::V3,
            std::vector<fiber::net::IpAddress> ips = {}) {
    fiber::nacos::NacosClientConfigParams params;
    params.server_ips = std::move(ips);
    if (params.server_ips.empty()) {
        params.server_ips.push_back(fiber::net::IpAddress::loopback_v4());
    }
    params.username = "user name";
    params.password = "p@ ss&";
    params.http_port = port;
    params.auth_api_version = version;
    auto result = fiber::nacos::NacosClientConfig::create(std::move(params));
    EXPECT_TRUE(result.has_value());
    return std::move(*result);
}

struct SnapshotOutcome {
    fiber::common::IoErr error = fiber::common::IoErr::Unknown;
    std::vector<NacosAuthSnapshot> snapshots;
};

DetachedTask collect_states_and_shutdown(NacosClient *client, NacosClient::AuthSubscriber *subscriber,
                                         std::vector<NacosAuthState> expected_states,
                                         std::promise<SnapshotOutcome> *promise) {
    SnapshotOutcome outcome;
    std::uint64_t received_version = 0;
    auto start = client->start();
    if (!start) {
        outcome.error = start.error();
        promise->set_value(std::move(outcome));
        fiber::event::EventLoop::current().stop();
        co_return;
    }

    for (const NacosAuthState expected: expected_states) {
        auto next = co_await fiber::async::timeout_for(
                [subscriber, received_version]() { return subscriber->next(received_version); }, 4s);
        if (!next || !next->value) {
            outcome.error = next ? fiber::common::IoErr::Invalid : next.error();
            co_await client->shutdown();
            promise->set_value(std::move(outcome));
            fiber::event::EventLoop::current().stop();
            co_return;
        }
        received_version = next->version;
        outcome.snapshots.push_back(*next->value);
        if (next->value->state != expected) {
            outcome.error = fiber::common::IoErr::Invalid;
            co_await client->shutdown();
            promise->set_value(std::move(outcome));
            fiber::event::EventLoop::current().stop();
            co_return;
        }
    }

    co_await client->shutdown();
    co_await client->shutdown();
    auto stopped = co_await fiber::async::timeout_for(
            [subscriber, received_version]() { return subscriber->next(received_version); }, 1s);
    if (!stopped || !stopped->value || stopped->value->state != NacosAuthState::Stopped) {
        outcome.error = stopped ? fiber::common::IoErr::Invalid : stopped.error();
    } else {
        outcome.snapshots.push_back(*stopped->value);
        outcome.error = fiber::common::IoErr::None;
    }
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
}

DetachedTask start_client(NacosClient *client, std::promise<fiber::common::IoErr> *promise) {
    auto result = client->start();
    promise->set_value(result ? fiber::common::IoErr::None : result.error());
    co_return;
}

DetachedTask shutdown_client(NacosClient *client, NacosClient::AuthSubscriber *subscriber,
                             std::promise<SnapshotOutcome> *promise) {
    SnapshotOutcome outcome;
    co_await client->shutdown();
    co_await client->shutdown();
    auto stopped = co_await fiber::async::timeout_for([subscriber]() { return subscriber->next(0); }, 1s);
    if (!stopped || !stopped->value) {
        outcome.error = stopped ? fiber::common::IoErr::Invalid : stopped.error();
    } else {
        outcome.snapshots.push_back(*stopped->value);
        outcome.error = stopped->value->state == NacosAuthState::Stopped ? fiber::common::IoErr::None
                                                                         : fiber::common::IoErr::Invalid;
    }
    promise->set_value(std::move(outcome));
    fiber::event::EventLoop::current().stop();
}

TEST(NacosClientTest, AuthSubscriberStartsWithoutPublishedSnapshot) {
    fiber::event::EventLoop loop;
    auto client_result = NacosClient::create(loop, make_config(8848));
    ASSERT_TRUE(client_result.has_value());

    auto subscriber = (*client_result)->subscribe_auth();
    auto snapshot = subscriber.current();
    EXPECT_EQ(snapshot.value, nullptr);
    EXPECT_EQ(snapshot.version, 0u);
}

TEST(NacosClientTest, LogsInWithV3AndBroadcastsReadyThenStopped) {
    ScriptedHttpServer server({ServerStep{
            .body = R"({"accessToken":"token-1","tokenTtl":30,"globalAdmin":true,"username":"nacos"})",
    }});
    ASSERT_TRUE(server.valid());

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(server.port()));
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber, {NacosAuthState::Ready}, &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.snapshots.size(), 2u);
    EXPECT_EQ(outcome.snapshots[0].access_token, "token-1");
    EXPECT_EQ(outcome.snapshots[0].username, "nacos");
    EXPECT_TRUE(outcome.snapshots[0].global_admin);
    EXPECT_EQ(outcome.snapshots[0].generation, 1u);
    EXPECT_EQ(outcome.snapshots[1].state, NacosAuthState::Stopped);

    ASSERT_TRUE(server.wait_for_requests(1, 1s));
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_TRUE(requests[0].starts_with("POST /nacos/v3/auth/user/login HTTP/1.1\r\n"));
    EXPECT_NE(requests[0].find("content-type: application/x-www-form-urlencoded\r\n"), std::string::npos);
    EXPECT_NE(requests[0].find("connection: close\r\n"), std::string::npos);
    EXPECT_TRUE(requests[0].ends_with("username=user+name&password=p%40+ss%26"));
}

TEST(NacosClientTest, AutoFallsBackFromV3ToLegacyV1) {
    ScriptedHttpServer server({
            ServerStep{.status = 404, .body = "{}"},
            ServerStep{.body = R"({"accessToken":"legacy-token","tokenTtl":30})"},
    });
    ASSERT_TRUE(server.valid());

    fiber::event::EventLoopGroup group(1);
    auto client_result =
            NacosClient::create(group.at(0), make_config(server.port(), fiber::nacos::NacosAuthApiVersion::Auto));
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber, {NacosAuthState::Ready}, &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.snapshots[0].access_token, "legacy-token");
    ASSERT_TRUE(server.wait_for_requests(2, 1s));
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(requests[0].starts_with("POST /nacos/v3/auth/user/login HTTP/1.1\r\n"));
    EXPECT_TRUE(requests[1].starts_with("POST /nacos/v1/auth/users/login HTTP/1.1\r\n"));
}

TEST(NacosClientTest, RetriesInitialFailureAndRecovers) {
    ScriptedHttpServer server({
            ServerStep{.status = 500, .body = "{}"},
            ServerStep{.body = R"({"accessToken":"recovered","tokenTtl":30})"},
    });
    ASSERT_TRUE(server.valid());

    fiber::nacos::NacosClientOptions options;
    options.retry_initial_delay = 20ms;
    options.retry_max_delay = 20ms;

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(server.port()), options);
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber,
                                           {NacosAuthState::Unavailable, NacosAuthState::Ready}, &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_GE(outcome.snapshots.size(), 3u);
    EXPECT_EQ(outcome.snapshots[0].last_error.code, NacosAuthErrorCode::HttpStatus);
    EXPECT_EQ(outcome.snapshots[0].last_error.http_status, 500);
    EXPECT_EQ(outcome.snapshots[1].access_token, "recovered");
}

TEST(NacosClientTest, RefreshFailureKeepsTokenUntilExpiry) {
    ScriptedHttpServer server({
            ServerStep{.body = R"({"accessToken":"short-lived","tokenTtl":1})"},
            ServerStep{.status = 500, .body = "{}"},
    });
    ASSERT_TRUE(server.valid());

    fiber::nacos::NacosClientOptions options;
    options.refresh_percent = 20;
    options.min_refresh_delay = 20ms;
    options.retry_initial_delay = 2s;
    options.retry_max_delay = 2s;

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(server.port()), options);
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber,
                                           {NacosAuthState::Ready, NacosAuthState::Ready, NacosAuthState::Unavailable},
                                           &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.snapshots.size(), 4u);
    EXPECT_EQ(outcome.snapshots[0].access_token, "short-lived");
    EXPECT_EQ(outcome.snapshots[1].state, NacosAuthState::Ready);
    EXPECT_EQ(outcome.snapshots[1].access_token, "short-lived");
    EXPECT_EQ(outcome.snapshots[1].last_error.code, NacosAuthErrorCode::HttpStatus);
    EXPECT_EQ(outcome.snapshots[2].state, NacosAuthState::Unavailable);
    EXPECT_TRUE(outcome.snapshots[2].access_token.empty());
    EXPECT_EQ(outcome.snapshots[2].last_error.code, NacosAuthErrorCode::TokenExpired);
}

TEST(NacosClientTest, SuccessfulRefreshPublishesNewGeneration) {
    ScriptedHttpServer server({
            ServerStep{.body = R"({"accessToken":"token-1","tokenTtl":1})"},
            ServerStep{.body = R"({"accessToken":"token-2","tokenTtl":30})"},
    });
    ASSERT_TRUE(server.valid());

    fiber::nacos::NacosClientOptions options;
    options.refresh_percent = 20;
    options.min_refresh_delay = 20ms;

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(server.port()), options);
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber, {NacosAuthState::Ready, NacosAuthState::Ready},
                                           &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.snapshots.size(), 3u);
    EXPECT_EQ(outcome.snapshots[0].access_token, "token-1");
    EXPECT_EQ(outcome.snapshots[0].generation, 1u);
    EXPECT_EQ(outcome.snapshots[1].access_token, "token-2");
    EXPECT_EQ(outcome.snapshots[1].generation, 2u);
}

TEST(NacosClientTest, BlockedRefreshExpiresTokenWithinRemainingLifetime) {
    ScriptedHttpServer server({
            ServerStep{.body = R"({"accessToken":"short-lived","tokenTtl":1})"},
            ServerStep{.hold_until_peer_close = true},
    });
    ASSERT_TRUE(server.valid());

    fiber::nacos::NacosClientOptions options;
    options.refresh_percent = 20;
    options.min_refresh_delay = 20ms;
    options.request_timeout = 10s;
    options.retry_initial_delay = 2s;
    options.retry_max_delay = 2s;

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(server.port()), options);
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    const auto started_at = std::chrono::steady_clock::now();
    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber,
                                           {NacosAuthState::Ready, NacosAuthState::Unavailable}, &promise);
    });

    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.snapshots.size(), 3u);
    EXPECT_EQ(outcome.snapshots[0].access_token, "short-lived");
    EXPECT_EQ(outcome.snapshots[1].state, NacosAuthState::Unavailable);
    EXPECT_TRUE(outcome.snapshots[1].access_token.empty());
    EXPECT_EQ(outcome.snapshots[1].last_error.code, NacosAuthErrorCode::TokenExpired);
    EXPECT_LT(elapsed, 2s);
    ASSERT_TRUE(server.wait_for_requests(2, 1s));
    EXPECT_TRUE(server.wait_for_peer_close(1s));
}

TEST(NacosClientTest, FallsBackAcrossConfiguredIpAddresses) {
    ScriptedHttpServer server({
            ServerStep{.body = R"({"accessToken":"fallback-token","tokenTtl":30})"},
    });
    ASSERT_TRUE(server.valid());

    std::vector<fiber::net::IpAddress> ips;
    ips.push_back(parse_ip("127.0.0.2"));
    ips.push_back(parse_ip("127.0.0.1"));

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(
            group.at(0), make_config(server.port(), fiber::nacos::NacosAuthApiVersion::V3, std::move(ips)));
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return collect_states_and_shutdown(client.get(), &subscriber, {NacosAuthState::Ready}, &promise);
    });

    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.snapshots[0].access_token, "fallback-token");
    ASSERT_TRUE(server.wait_for_requests(1, 1s));
}

TEST(NacosClientTest, ReportsInvalidLoginResponses) {
    struct TestCase {
        std::string body;
        std::size_t max_response_bytes = 64 * 1024;
        NacosAuthErrorCode expected_error = NacosAuthErrorCode::InvalidJson;
    };

    const std::vector<TestCase> cases{
            TestCase{.body = "{bad", .expected_error = NacosAuthErrorCode::InvalidJson},
            TestCase{.body = R"({"tokenTtl":30})", .expected_error = NacosAuthErrorCode::MissingAccessToken},
            TestCase{.body = R"({"accessToken":"token","tokenTtl":0})",
                     .expected_error = NacosAuthErrorCode::InvalidTokenTtl},
            TestCase{.body = R"({"accessToken":"too-long","tokenTtl":30})",
                     .max_response_bytes = 8,
                     .expected_error = NacosAuthErrorCode::ResponseTooLarge},
    };

    for (const TestCase &test_case: cases) {
        SCOPED_TRACE(static_cast<int>(test_case.expected_error));
        ScriptedHttpServer server({ServerStep{.body = test_case.body}});
        ASSERT_TRUE(server.valid());

        fiber::nacos::NacosClientOptions options;
        options.max_auth_response_bytes = test_case.max_response_bytes;

        fiber::event::EventLoopGroup group(1);
        auto client_result = NacosClient::create(group.at(0), make_config(server.port()), options);
        ASSERT_TRUE(client_result.has_value());
        std::unique_ptr<NacosClient> client = std::move(*client_result);
        auto subscriber = client->subscribe_auth();
        std::promise<SnapshotOutcome> promise;
        auto future = promise.get_future();

        group.start();
        fiber::async::spawn(group.at(0), [&]() {
            return collect_states_and_shutdown(client.get(), &subscriber, {NacosAuthState::Unavailable}, &promise);
        });

        ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
        SnapshotOutcome outcome = future.get();
        group.join();

        ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
        ASSERT_EQ(outcome.snapshots.size(), 2u);
        EXPECT_EQ(outcome.snapshots[0].last_error.code, test_case.expected_error);
    }
}

TEST(NacosClientTest, ShutdownBeforeStartPublishesStopped) {
    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(8848));
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();
    std::promise<SnapshotOutcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return shutdown_client(client.get(), &subscriber, &promise); });

    ASSERT_EQ(future.wait_for(2s), std::future_status::ready);
    SnapshotOutcome outcome = future.get();
    group.join();

    ASSERT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.snapshots.size(), 1u);
    EXPECT_EQ(outcome.snapshots[0].state, NacosAuthState::Stopped);
}

TEST(NacosClientTest, ShutdownWaitsOnlyForCurrentBoundedHttpOperation) {
    ScriptedHttpServer server({
            ServerStep{.hold_until_peer_close = true},
    });
    ASSERT_TRUE(server.valid());

    fiber::nacos::NacosClientOptions options;
    options.request_timeout = 150ms;

    fiber::event::EventLoopGroup group(1);
    auto client_result = NacosClient::create(group.at(0), make_config(server.port()), options);
    ASSERT_TRUE(client_result.has_value());
    std::unique_ptr<NacosClient> client = std::move(*client_result);
    auto subscriber = client->subscribe_auth();

    std::promise<fiber::common::IoErr> start_promise;
    auto start_future = start_promise.get_future();
    group.start();
    fiber::async::spawn(group.at(0), [&]() { return start_client(client.get(), &start_promise); });
    ASSERT_EQ(start_future.wait_for(2s), std::future_status::ready);
    ASSERT_EQ(start_future.get(), fiber::common::IoErr::None);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));

    std::promise<SnapshotOutcome> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();
    const auto started_at = std::chrono::steady_clock::now();
    fiber::async::spawn(group.at(0), [&]() { return shutdown_client(client.get(), &subscriber, &shutdown_promise); });

    ASSERT_EQ(shutdown_future.wait_for(2s), std::future_status::ready);
    SnapshotOutcome outcome = shutdown_future.get();
    const auto elapsed = std::chrono::steady_clock::now() - started_at;
    group.join();

    EXPECT_EQ(outcome.error, fiber::common::IoErr::None);
    ASSERT_EQ(outcome.snapshots.size(), 1u);
    EXPECT_EQ(outcome.snapshots[0].state, NacosAuthState::Stopped);
    EXPECT_LT(elapsed, 1s);
    EXPECT_TRUE(server.wait_for_peer_close(1s));
}

TEST(NacosClientTest, RejectsInvalidOptions) {
    fiber::event::EventLoop loop;
    fiber::nacos::NacosClientOptions options;
    options.refresh_percent = 100;
    auto result = NacosClient::create(loop, make_config(8848), options);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, fiber::nacos::NacosCreateErrorCode::InvalidOptions);
}

} // namespace
