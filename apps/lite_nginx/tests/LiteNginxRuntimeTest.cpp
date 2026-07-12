#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "config/ConfigLoader.h"
#include "event/EventLoop.h"
#include "runtime/RuntimeBuilder.h"
#include "runtime/ServerLauncher.h"

namespace {

using namespace std::chrono_literals;

struct ShutdownOp {
    fiber::event::EventLoop::NotifyEntry entry;
    fiber::lite_nginx::runtime::ServerLauncher *launcher = nullptr;
    fiber::event::EventLoop *loop = nullptr;

    static void on_run(ShutdownOp *self) noexcept {
        if (self->launcher) {
            self->launcher->close();
        }
        if (self->loop) {
            self->loop->stop();
        }
    }
};

std::string recv_http_response(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            return out.substr(0, header_end + content_length);
        }

        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<std::size_t>(rc));

        if (header_end != std::string::npos) {
            continue;
        }

        std::size_t pos = out.find("\r\n\r\n");
        if (pos == std::string::npos) {
            continue;
        }
        header_end = pos + 4;

        std::size_t cl_pos = out.find("Content-Length:");
        if (cl_pos == std::string::npos) {
            return out;
        }
        cl_pos += sizeof("Content-Length:") - 1;
        while (cl_pos < pos && out[cl_pos] == ' ') {
            ++cl_pos;
        }
        std::size_t cl_end = out.find("\r\n", cl_pos);
        if (cl_end == std::string::npos) {
            return out;
        }
        content_length = static_cast<std::size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
    }
}

std::uint16_t reserve_loopback_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        ::close(fd);
        return 0;
    }

    std::uint16_t port = ntohs(bound.sin_port);
    ::close(fd);
    return port;
}

int connect_client(std::uint16_t port) {
    int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (client < 0) {
        return -1;
    }

    timeval tv{};
    tv.tv_sec = 3;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(client);
        return -1;
    }
    return client;
}

std::string read_http_request(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    std::size_t header_end = std::string::npos;
    std::size_t content_length = 0;

    for (;;) {
        if (header_end != std::string::npos && out.size() >= header_end + content_length) {
            return out.substr(0, header_end + content_length);
        }

        ssize_t rc = ::recv(fd, buf.data(), buf.size(), 0);
        if (rc <= 0) {
            return out;
        }
        out.append(buf.data(), static_cast<std::size_t>(rc));

        if (header_end != std::string::npos) {
            continue;
        }

        std::size_t pos = out.find("\r\n\r\n");
        if (pos == std::string::npos) {
            continue;
        }
        header_end = pos + 4;

        std::size_t cl_pos = out.find("Content-Length:");
        if (cl_pos == std::string::npos) {
            return out;
        }
        cl_pos += sizeof("Content-Length:") - 1;
        while (cl_pos < pos && out[cl_pos] == ' ') {
            ++cl_pos;
        }
        std::size_t cl_end = out.find("\r\n", cl_pos);
        if (cl_end == std::string::npos) {
            return out;
        }
        content_length = static_cast<std::size_t>(std::stoul(out.substr(cl_pos, cl_end - cl_pos)));
    }
}

class SingleRequestUpstream {
public:
    SingleRequestUpstream(std::string response, std::promise<std::string> *request_promise) :
        response_(std::move(response)), request_promise_(request_promise) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "bind failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        if (::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                if (request_promise_) {
                    request_promise_->set_value({});
                }
                return;
            }

            std::string request = read_http_request(client);
            if (request_promise_) {
                request_promise_->set_value(request);
            }

            const char *data = response_.data();
            std::size_t remaining = response_.size();
            while (remaining > 0) {
                ssize_t rc = ::send(client, data, remaining, 0);
                if (rc <= 0) {
                    break;
                }
                data += static_cast<std::size_t>(rc);
                remaining -= static_cast<std::size_t>(rc);
            }
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        });
    }

    ~SingleRequestUpstream() {
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    int listener_fd_ = -1;
    std::uint16_t port_ = 0;
    std::string response_;
    std::promise<std::string> *request_promise_ = nullptr;
    std::thread thread_{};
};

class KeepAliveUpstream {
public:
    KeepAliveUpstream(std::array<std::string, 2> responses,
                      std::array<std::promise<std::string> *, 2> request_promises) :
        responses_(std::move(responses)), request_promises_(request_promises) {
        listener_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        EXPECT_GE(listener_fd_, 0);
        if (listener_fd_ < 0) {
            return;
        }

        int yes = 1;
        ::setsockopt(listener_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(0);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            ADD_FAILURE() << "bind failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        if (::listen(listener_fd_, 16) != 0) {
            ADD_FAILURE() << "listen failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listener_fd_, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
            ADD_FAILURE() << "getsockname failed: " << errno;
            ::close(listener_fd_);
            listener_fd_ = -1;
            return;
        }
        port_ = ntohs(bound.sin_port);

        thread_ = std::thread([this]() {
            std::size_t served = 0;
            while (listener_fd_ >= 0 && served < responses_.size()) {
                int client = ::accept4(listener_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (client < 0) {
                    break;
                }
                ++accept_count_;

                while (served < responses_.size()) {
                    std::string request = read_http_request(client);
                    if (request.empty()) {
                        break;
                    }
                    if (request_promises_[served]) {
                        request_promises_[served]->set_value(request);
                    }

                    const char *data = responses_[served].data();
                    std::size_t remaining = responses_[served].size();
                    while (remaining > 0) {
                        ssize_t rc = ::send(client, data, remaining, 0);
                        if (rc <= 0) {
                            break;
                        }
                        data += static_cast<std::size_t>(rc);
                        remaining -= static_cast<std::size_t>(rc);
                    }
                    ++served;
                    if (remaining > 0) {
                        break;
                    }
                }

                ::shutdown(client, SHUT_RDWR);
                ::close(client);
            }
        });
    }

    ~KeepAliveUpstream() {
        if (listener_fd_ >= 0) {
            ::shutdown(listener_fd_, SHUT_RDWR);
            ::close(listener_fd_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] int accept_count() const noexcept { return accept_count_.load(); }

private:
    int listener_fd_ = -1;
    std::uint16_t port_ = 0;
    std::array<std::string, 2> responses_{};
    std::array<std::promise<std::string> *, 2> request_promises_{};
    std::atomic<int> accept_count_{0};
    std::thread thread_{};
};

class RuntimeHarness {
public:
    explicit RuntimeHarness(const fiber::lite_nginx::runtime::RuntimeConfig &runtime) : launcher_(loop_) {
        auto start_result = launcher_.start(runtime);
        if (!start_result.has_value()) {
            ADD_FAILURE() << start_result.error().message;
            return;
        }
        if (launcher_.bound_listeners().size() != 1U) {
            ADD_FAILURE() << "unexpected listener count";
            return;
        }
        thread_ = std::thread([this]() { loop_.run(); });
    }

    ~RuntimeHarness() {
        ShutdownOp shutdown{
                .launcher = &launcher_,
                .loop = &loop_,
        };
        loop_.post<ShutdownOp, &ShutdownOp::entry, &ShutdownOp::on_run>(shutdown);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const { return launcher_.bound_listeners().front().address.port(); }

private:
    fiber::event::EventLoop loop_;
    fiber::lite_nginx::runtime::ServerLauncher launcher_;
    std::thread thread_{};
};

TEST(LiteNginxRuntimeTest, RejectsDuplicateServerNames) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;

    server {
        server_name same.test;
        location /* {
            proxy_pass http://127.0.0.1:9001;
        }
    }

    server {
        server_name same.test;
        location /* {
            proxy_pass http://127.0.0.1:9002;
        }
    }
}
)",
                                                                            "dup_server_name.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("duplicate server_name"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, BuildsHttp3ListenerRuntime) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8443 ssl http3;

    server {
        server_name localhost;
        certificate /tmp/localhost.crt;
        certificate_key /tmp/localhost.key;
        location /* {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)",
                                                                            "http3_listener.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_EQ(runtime->listeners.size(), 1u);
    EXPECT_TRUE(runtime->listeners[0].tls);
    EXPECT_TRUE(runtime->listeners[0].http3);
    EXPECT_EQ(runtime->listeners[0].http3_alt_svc, "h3=\":8443\"; ma=86400");
}

TEST(LiteNginxRuntimeTest, ProxiesDirectRouteMatcherLocation) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nproxied",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /api/:id {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header Host backend.internal;
        }
    }
}
)";
    std::size_t listen_marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(listen_marker, std::string::npos);
    config_text.replace(listen_marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    std::size_t upstream_marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(upstream_marker, std::string::npos);
    config_text.replace(upstream_marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_proxy.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /api/42?x=1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("proxied"), std::string::npos);
    EXPECT_NE(proxied_request.find("GET /api/42?x=1 HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("Host: backend.internal\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("Connection: close\r\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, SuppressesOverriddenAndConnectionDeclaredRequestHeaders) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: text/plain\r\n\r\nok",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header Host backend.internal;
            proxy_set_header X-Test replaced;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_skip_headers.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\n"
                           "Host: localhost\r\n"
                           "Connection: close, x-hop\r\n"
                           "X-Hop: drop-me\r\n"
                           "X-Test: original\r\n"
                           "X-Preserve: keep-me\r\n"
                           "\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("Host: backend.internal\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("X-Test: replaced\r\n"), std::string::npos);
    EXPECT_NE(proxied_request.find("X-Preserve: keep-me\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("Host: localhost\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("Connection: close"), std::string::npos);
    EXPECT_EQ(proxied_request.find("X-Hop: drop-me\r\n"), std::string::npos);
    EXPECT_EQ(proxied_request.find("X-Test: original\r\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, RoutesNamedUpstreamAndSelectsServerByHost) {
    std::promise<std::string> api_request;
    auto api_future = api_request.get_future();
    SingleRequestUpstream api_upstream("HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\napi",
                                       &api_request);
    ASSERT_NE(api_upstream.port(), 0);

    std::promise<std::string> other_request;
    auto other_future = other_request.get_future();
    SingleRequestUpstream other_upstream(
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nother", &other_request);
    ASSERT_NE(other_upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:API_UPSTREAM_PORT;
    }

    server {
        server_name api.local;
        location /files/*tail {
            proxy_pass http://backend;
        }
    }

    server {
        server_name other.local;
        location /* {
            proxy_pass http://127.0.0.1:OTHER_UPSTREAM_PORT;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("API_UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("API_UPSTREAM_PORT") - 1, std::to_string(api_upstream.port()));

    marker = config_text.find("OTHER_UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("OTHER_UPSTREAM_PORT") - 1, std::to_string(other_upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_named_upstream.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char api_request_text[] = "GET /files/a/b HTTP/1.1\r\nHost: api.local\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, api_request_text, sizeof(api_request_text) - 1, 0),
              static_cast<ssize_t>(sizeof(api_request_text) - 1));
    std::string api_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char other_request_text[] = "GET /whatever HTTP/1.1\r\nHost: other.local\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, other_request_text, sizeof(other_request_text) - 1, 0),
              static_cast<ssize_t>(sizeof(other_request_text) - 1));
    std::string other_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(api_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(other_future.wait_for(3s), std::future_status::ready);
    std::string proxied_api_request = api_future.get();
    std::string proxied_other_request = other_future.get();

    EXPECT_NE(api_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(api_response.find("api"), std::string::npos);
    EXPECT_NE(other_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(other_response.find("other"), std::string::npos);
    EXPECT_NE(proxied_api_request.find("GET /files/a/b HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(proxied_other_request.find("GET /whatever HTTP/1.1\r\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, Returns404WhenNoRouteMatches) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:PORT;

    server {
        server_name localhost;
        location /api/:id {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)";
    std::size_t marker = config_text.find("PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, 4, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_404.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /miss HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 404 Not Found\r\n"), std::string::npos);
    EXPECT_NE(response.find("404 Not Found\n"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ReusesNamedUpstreamConnectionsWithKeepalive) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:UPSTREAM_PORT;
    }

    connection_pool {
        keepalive_size 2;
        keepalive_timeout 30s;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass http://backend;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_keepalive.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);
    std::string first_proxied_request = first_future.get();
    std::string second_proxied_request = second_future.get();

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(first_response.find("first"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("second"), std::string::npos);
    EXPECT_NE(first_proxied_request.find("GET /first HTTP/1.1\r\n"), std::string::npos);
    EXPECT_NE(second_proxied_request.find("GET /second HTTP/1.1\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

// steal off must still pool per-loop: with worker_processes 1 there is one loop, so two sequential
// requests reuse one upstream connection (accept_count == 1). This exercises the LocalHttp1ConnectionPoolSet
// wiring through the unified acquire_and_connect path.
TEST(LiteNginxRuntimeTest, ReusesConnectionsWithStealOff) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:UPSTREAM_PORT;
    }

    connection_pool {
        keepalive_size 2;
        keepalive_timeout 30s;
        steal off;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass http://backend;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_steal_off.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    EXPECT_FALSE(runtime->connection_pool.steal);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);
    first_future.get();
    second_future.get();

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

TEST(LiteNginxRuntimeTest, PropagatesConnectionPoolSizingToRuntime) {
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;

    connection_pool {
        keepalive_size 4;
        keepalive_timeout 30s;
        max_idle_total 128;
        initial_group_capacity 8;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)";
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_sizing.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    EXPECT_EQ(runtime->connection_pool.keepalive_size, 4u);
    EXPECT_EQ(runtime->connection_pool.max_idle_total, 128u);
    EXPECT_EQ(runtime->connection_pool.initial_group_capacity, 8u);
}

TEST(LiteNginxRuntimeTest, DoesNotPoolDirectProxyPassTargets) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_direct_short.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 2);
}

TEST(LiteNginxRuntimeTest, StealsNamedUpstreamConnectionsAcrossWorkersWhenEnabled) {
    std::promise<std::string> first_upstream_request;
    std::promise<std::string> second_upstream_request;
    auto first_future = first_upstream_request.get_future();
    auto second_future = second_upstream_request.get_future();
    KeepAliveUpstream upstream(
            {
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nfirst",
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nContent-Type: text/plain\r\nConnection: "
                    "keep-alive\r\n\r\nsecond",
            },
            {&first_upstream_request, &second_upstream_request});
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 2;
http {
    listen 127.0.0.1:LISTEN_PORT;

    upstream backend {
        server 127.0.0.1:UPSTREAM_PORT;
    }

    connection_pool {
        keepalive_size 2;
        keepalive_timeout 30s;
    }

    server {
        server_name localhost;
        location /* {
            proxy_pass http://backend;
        }
    }
}
)";
    std::size_t marker = config_text.find("LISTEN_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("LISTEN_PORT") - 1, std::to_string(port));

    marker = config_text.find("UPSTREAM_PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, sizeof("UPSTREAM_PORT") - 1, std::to_string(upstream.port()));

    auto config =
            fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_stealable_keepalive.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char first_request[] = "GET /first HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, first_request, sizeof(first_request) - 1, 0),
              static_cast<ssize_t>(sizeof(first_request) - 1));
    std::string first_response = recv_http_response(client);
    ::close(client);

    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char second_request[] = "GET /second HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, second_request, sizeof(second_request) - 1, 0),
              static_cast<ssize_t>(sizeof(second_request) - 1));
    std::string second_response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(first_future.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(second_future.wait_for(3s), std::future_status::ready);

    EXPECT_NE(first_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(second_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_EQ(upstream.accept_count(), 1);
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationServesScriptResponse) {
    const std::string script_path = "/tmp/lite_nginx_script_location_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, {msg: \"hello-script\", path: req.getPath()});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;

    server {
        server_name localhost;
        location /* {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "script_location.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_TRUE(runtime->script_library != nullptr);
    ASSERT_EQ(runtime->servers[0].locations.size(), 1u);
    ASSERT_TRUE(runtime->servers[0].locations[0].script != nullptr);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /hello HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"msg\":\"hello-script\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"path\":\"/hello\""), std::string::npos) << response;

    ::unlink(script_path.c_str());
}

// Drives a single GET /x through a script_file location whose script is `script_body`, and
// returns the raw HTTP/1.1 response. Used to check how run_script synthesizes a response from
// the script outcome (Value -> 200+json, Void -> 204, Exception -> 500+json, Abort -> 500+json)
// when the script never called resp.* / http.proxyPass itself.
std::string run_script_result_response(std::string_view script_body) {
    const std::string script_path = "/tmp/lite_nginx_script_result_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        if (!file.good()) {
            return {};
        }
        file << script_body;
    }

    std::uint16_t port = reserve_loopback_port();
    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /* {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "script_result.conf");
    if (!config.has_value()) {
        ::unlink(script_path.c_str());
        return {};
    }
    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    if (!runtime.has_value()) {
        ::unlink(script_path.c_str());
        return {};
    }
    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    const char request[] = "GET /x HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    (void) ::send(client, request, sizeof(request) - 1, 0);
    std::string response = recv_http_response(client);
    ::close(client);
    ::unlink(script_path.c_str());
    return response;
}

// A script that returns a Value is served as 200 + a JSON body encoding that value.
TEST(LiteNginxRuntimeTest, ScriptReturnValueServes200Json) {
    std::string response = run_script_result_response("return {a: 1, b: [2, 3]};");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"a\":1"), std::string::npos) << response;
    EXPECT_NE(response.find("\"b\":[2,3]"), std::string::npos) << response;
}

// A script that ends without producing a value (bare `return;` / fall-through) is Void and
// served as 204 No Content.
TEST(LiteNginxRuntimeTest, ScriptReturnVoidServes204) {
    std::string response = run_script_result_response("var x = 42;");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 204 No Content\r\n"), std::string::npos) << response;
}

// An uncaught tagged TypeError (no heap payload) is served as 500 + {"error":"TypeError"}.
TEST(LiteNginxRuntimeTest, ScriptTaggedExceptionServes500JsonErrorName) {
    std::string response = run_script_result_response("hash.md5(123);");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 500"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"error\":\"TypeError\""), std::string::npos) << response;
}

// An uncaught heap exception (JSON.parse failure -> SyntaxError) is served as 500 + the
// exception's serialized form, which carries its name.
TEST(LiteNginxRuntimeTest, ScriptHeapExceptionServes500JsonWithName) {
    std::string response = run_script_result_response("JSON.parse(\"{bad\");");
    ASSERT_FALSE(response.empty()) << "no response";
    EXPECT_NE(response.find("HTTP/1.1 500"), std::string::npos) << response;
    EXPECT_NE(response.find("application/json"), std::string::npos) << response;
    EXPECT_NE(response.find("\"name\":\"SyntaxError\""), std::string::npos) << response;
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationRejectsMissingFile) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server {
        server_name localhost;
        location /* { script_file /tmp/lite_nginx_does_not_exist_999999.js; }
    }
}
)",
                                                                            "missing_script.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("script_file not found"), std::string::npos);
}

// A relative script_file (resolved against the config file's directory at parse time) is
// opened and compiled by RuntimeBuilder regardless of the process pwd. This exercises the
// full path-resolution + open + compile pipeline end to end via load_from_file.
TEST(LiteNginxRuntimeTest, RelativeScriptFileCompilesAfterResolution) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "lite_nginx_rel_script_e2e";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "scripts", ec);
    {
        std::ofstream f(dir / "scripts" / "x.js", std::ios::binary | std::ios::trunc);
        f << "resp.sendJson(200, {msg: \"hello-relative\", path: req.getPath()});";
    }
    {
        std::ofstream f(dir / "main.conf", std::ios::binary | std::ios::trunc);
        f << R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server { server_name localhost; location /* { script_file scripts/x.js; } }
}
)";
    }

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_file((dir / "main.conf").string());
    ASSERT_TRUE(config.has_value()) << config.error().message;
    // Resolved to an absolute path under the config directory (not the bare "scripts/x.js").
    const auto &script_file = config->http.servers[0].locations[0].script_file;
    EXPECT_EQ(script_file.front(), '/') << script_file;
    EXPECT_NE(script_file.find("scripts/x.js"), std::string::npos) << script_file;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_EQ(runtime->servers[0].locations.size(), 1u);
    EXPECT_TRUE(runtime->servers[0].locations[0].script != nullptr);

    fs::remove_all(dir, ec);
}

TEST(LiteNginxRuntimeTest, ScriptFileAndProxyPassAreMutuallyExclusive) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server {
        server_name localhost;
        location /* {
            proxy_pass http://127.0.0.1:9001;
            script_file /tmp/x.js;
        }
    }
}
)",
                                                                            "mutual_exclusive.conf");
    ASSERT_FALSE(config.has_value());
    EXPECT_NE(config.error().message.find("only one of proxy_pass or script_file"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationResolvesPathVar) {
    const std::string script_path = "/tmp/lite_nginx_path_var_resolve_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        // $path.id is validated at compile time against the route pattern /users/:id, and
        // resolved at request time from the matched path capture.
        file << "resp.sendJson(200, {id: $path.id, uri: $req.uri, method: $req.method});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /users/:id {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "path_var.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;
    ASSERT_TRUE(runtime->servers[0].locations[0].script != nullptr);
    ASSERT_TRUE(runtime->servers[0].locations[0].route_lib != nullptr);

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /users/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\"id\":\"42\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"uri\":\"/users/42\""), std::string::npos) << response;
    EXPECT_NE(response.find("\"method\":\"GET\""), std::string::npos) << response;

    ::unlink(script_path.c_str());
}

TEST(LiteNginxRuntimeTest, ScriptFileLocationRejectsUnknownPathVar) {
    // $path.missing is not a capture of /users/:id, so the script must fail to compile at
    // runtime-build time with "constant not found".
    const std::string script_path = "/tmp/lite_nginx_path_var_unknown_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, $path.missing);";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /users/:id {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "path_var_unknown.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("constant not found"), std::string::npos) << runtime.error().message;

    ::unlink(script_path.c_str());
}

// A bare `location /foo` (no `:param`/`*`) matches exactly that path -- it is NOT a prefix.
// `/foo` hits the script; `/foo/bar` matches no location and returns 404.
TEST(LiteNginxRuntimeTest, ScriptFileLocationBarePatternMatchesExactly) {
    const std::string script_path = "/tmp/lite_nginx_bare_pattern_exact_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, {hit: \"foo\"});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /foo {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "bare_pattern.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    // /foo matches the bare static location exactly.
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char foo_request[] = "GET /foo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, foo_request, sizeof(foo_request) - 1, 0), static_cast<ssize_t>(sizeof(foo_request) - 1));
    std::string foo_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(foo_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << foo_response;
    EXPECT_NE(foo_response.find("\"hit\":\"foo\""), std::string::npos) << foo_response;

    // /foo/bar does not match /foo (exact, not prefix) and there is no catch-all -> 404.
    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char sub_request[] = "GET /foo/bar HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, sub_request, sizeof(sub_request) - 1, 0), static_cast<ssize_t>(sizeof(sub_request) - 1));
    std::string sub_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(sub_response.find("HTTP/1.1 404"), std::string::npos) << sub_response;

    ::unlink(script_path.c_str());
}

// `location /` matches only the root path `/` -- it is NOT a catch-all (use `/*` for
// that). Locks the absence of the old `/` -> `/*` rewrite.
TEST(LiteNginxRuntimeTest, LocationRootPatternMatchesOnlyRoot) {
    const std::string script_path = "/tmp/lite_nginx_root_pattern_exact_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "resp.sendJson(200, {hit: \"root\"});";
    }

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            script_file SCRIPT_PATH;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "root_pattern.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    // / matches the root location exactly.
    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char root_request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, root_request, sizeof(root_request) - 1, 0),
              static_cast<ssize_t>(sizeof(root_request) - 1));
    std::string root_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(root_response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << root_response;
    EXPECT_NE(root_response.find("\"hit\":\"root\""), std::string::npos) << root_response;

    // /anything does not match / (exact, not catch-all) -> 404.
    client = connect_client(harness.port());
    ASSERT_GE(client, 0);
    const char sub_request[] = "GET /anything HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, sub_request, sizeof(sub_request) - 1, 0), static_cast<ssize_t>(sizeof(sub_request) - 1));
    std::string sub_response = recv_http_response(client);
    ::close(client);
    EXPECT_NE(sub_response.find("HTTP/1.1 404"), std::string::npos) << sub_response;

    ::unlink(script_path.c_str());
}

// vars.js (shipped under conf/scripts/) demonstrates all five route-variable constants
// ($path/$query/$header/$cookie/$req) on a /api/:id route. This loads the actual shipped
// file -- not a /tmp copy -- so it cannot bit-rot, and asserts every namespace end to end
// including the absent -> null contract.
TEST(LiteNginxRuntimeTest, ScriptFileVarsJsRouteVariables) {
    const std::string vars_js = std::string(FIBER_LITE_NGINX_SOURCE_DIR) + "/conf/scripts/vars.js";

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /api/:id {
            script_file VARS_JS;
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("VARS_JS"), sizeof("VARS_JS") - 1, vars_js);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "vars_js.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    // Present values across all five namespaces.
    {
        int client = connect_client(harness.port());
        ASSERT_GE(client, 0);
        const char request[] = "GET /api/42?src=web HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "X-Forwarded-For: 1.2.3.4\r\n"
                               "Cookie: session=abc\r\n"
                               "Connection: close\r\n"
                               "\r\n";
        ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
        std::string response = recv_http_response(client);
        ::close(client);

        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
        EXPECT_NE(response.find("\"id\":\"42\""), std::string::npos) << response; // $path
        EXPECT_NE(response.find("\"src\":\"web\""), std::string::npos) << response; // $query
        EXPECT_NE(response.find("\"clientIp\":\"1.2.3.4\""), std::string::npos) << response; // $header (-/_ fold)
        EXPECT_NE(response.find("\"session\":\"abc\""), std::string::npos) << response; // $cookie
        EXPECT_NE(response.find("\"uri\":\"/api/42?src=web\""), std::string::npos) << response; // $req.uri
        EXPECT_NE(response.find("\"method\":\"GET\""), std::string::npos) << response; // $req.method
        EXPECT_NE(response.find("\"path\":\"/api/42\""), std::string::npos) << response; // $req.path
        EXPECT_NE(response.find("\"queryStr\":\"src=web\""), std::string::npos) << response; // $req.query
    }

    // Absent -> null (not error, not undefined): $query/$header/$cookie missing; $req.query
    // is the empty raw query string.
    {
        int client = connect_client(harness.port());
        ASSERT_GE(client, 0);
        const char request[] = "GET /api/7 HTTP/1.1\r\n"
                               "Host: localhost\r\n"
                               "Connection: close\r\n"
                               "\r\n";
        ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));
        std::string response = recv_http_response(client);
        ::close(client);

        EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
        EXPECT_NE(response.find("\"id\":\"7\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"src\":null"), std::string::npos) << response;
        EXPECT_NE(response.find("\"clientIp\":null"), std::string::npos) << response;
        EXPECT_NE(response.find("\"session\":null"), std::string::npos) << response;
        EXPECT_NE(response.find("\"uri\":\"/api/7\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"method\":\"GET\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"path\":\"/api/7\""), std::string::npos) << response;
        EXPECT_NE(response.find("\"queryStr\":\"\""), std::string::npos) << response;
    }
}

// `directive svc = http "@backend"; svc.request({...})` issues an upstream request and returns
// {status, headers?, body}.
TEST(LiteNginxRuntimeTest, HttpRequestFetchesUpstreamResponse) {
    const std::string script_path = "/tmp/lite_nginx_http_request_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"@backend\";\n"
                "let r = svc.request({path: \"/x\", includeHeaders: true});\n"
                "resp.sendJson(200, {status: r.status, headers: r.headers});";
    }

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nproxied",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http_request.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\"status\":200"), std::string::npos) << response;
    EXPECT_NE(response.find("\"Content-Type\":\"text/plain\""), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("GET /x HTTP/1.1"), std::string::npos) << proxied_request;

    ::unlink(script_path.c_str());
}

// `directive svc = http "@backend"; svc.proxyPass({})` forwards the inbound request to the
// upstream and copies the upstream response back to the client.
TEST(LiteNginxRuntimeTest, HttpProxyPassForwardsRequestAndResponse) {
    const std::string script_path = "/tmp/lite_nginx_http_proxy_pass_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"@backend\";\n"
                "svc.proxyPass({});";
    }

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain\r\n\r\nproxied",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    upstream backend { server 127.0.0.1:UPSTREAM_PORT; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http_proxy_pass.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET /api/42 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("proxied"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("GET /api/42 HTTP/1.1"), std::string::npos) << proxied_request;

    ::unlink(script_path.c_str());
}

// `directive svc = http "http://127.0.0.1:PORT";` binds a script handle to an ad-hoc IP-literal URL
// target; svc.request then resolves to the bound target.
TEST(LiteNginxRuntimeTest, HttpDirectiveBindsUrlTarget) {
    const std::string script_path = "/tmp/lite_nginx_http_directive_test.js";
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"http://127.0.0.1:UPSTREAM_PORT\";\n"
                "let r = svc.request({path: \"/x\"});\n"
                "resp.sendJson(200, {status: r.status});";
    }

    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: text/plain\r\n\r\nok",
                                   &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    connection_pool { keepalive_size 8; keepalive_timeout 30s; }
    server {
        server_name localhost;
        location /* { script_file SCRIPT_PATH; }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("SCRIPT_PATH"), sizeof("SCRIPT_PATH") - 1, script_path);

    // Substitute the upstream port into the script (the directive binds to the URL literal).
    {
        std::ofstream file(script_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file.good());
        file << "directive svc = http \"http://127.0.0.1:" << upstream.port()
             << "\";\n"
                "let r = svc.request({path: \"/x\"});\n"
                "resp.sendJson(200, {status: r.status});";
    }

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "http_directive.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("\"status\":200"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("GET /x HTTP/1.1"), std::string::npos) << proxied_request;

    ::unlink(script_path.c_str());
}

// ${...} proxy_set_header values compile against the location's RouteScriptLibrary.
TEST(LiteNginxRuntimeTest, ProxySetHeaderTemplateCompiles) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            proxy_pass http://127.0.0.1:9001;
            proxy_set_header X-Original-Host "${$header.host}";
            proxy_set_header X-Static "literal";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "tmpl_compile.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    const auto &loc = runtime->servers[0].locations[0];
    ASSERT_EQ(loc.set_headers.size(), 2u);
    EXPECT_EQ(loc.set_headers[0].name, "X-Original-Host");
    EXPECT_EQ(loc.set_headers[1].name, "X-Static");
    // Template header has a compiled script; static header does not.
    EXPECT_TRUE(loc.set_headers[0].template_script != nullptr);
    EXPECT_TRUE(loc.set_headers[1].template_script == nullptr);
    EXPECT_FALSE(loc.set_headers[0].template_script->contains_async());
}

// $path.<unknown> in a template is a compile-time error (the name is not a route capture).
TEST(LiteNginxRuntimeTest, ProxySetHeaderTemplateRejectsUnknownPathVar) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location /users/:id {
            proxy_pass http://127.0.0.1:9001;
            proxy_set_header X "${$path.missing}";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "tmpl_bad_path.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("constant not found"), std::string::npos) << runtime.error().message;
}

// End-to-end: ${$header.host} evaluates per request from the inbound Host header and is sent
// to the upstream as the templated header value.
TEST(LiteNginxRuntimeTest, ProxySetHeaderTemplateEvaluatesPerRequest) {
    std::promise<std::string> upstream_request;
    auto upstream_future = upstream_request.get_future();
    SingleRequestUpstream upstream("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", &upstream_request);
    ASSERT_NE(upstream.port(), 0);

    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:LISTEN_PORT;
    server {
        server_name localhost;
        location / {
            proxy_pass http://127.0.0.1:UPSTREAM_PORT;
            proxy_set_header X-Original-Host "${$header.host}";
        }
    }
}
)";
    config_text.replace(config_text.find("LISTEN_PORT"), sizeof("LISTEN_PORT") - 1, std::to_string(port));
    config_text.replace(config_text.find("UPSTREAM_PORT"), sizeof("UPSTREAM_PORT") - 1,
                        std::to_string(upstream.port()));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "tmpl_eval.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value()) << runtime.error().message;

    RuntimeHarness harness(*runtime);

    int client = connect_client(harness.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ASSERT_EQ(upstream_future.wait_for(3s), std::future_status::ready);
    std::string proxied_request = upstream_future.get();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos) << response;
    EXPECT_NE(proxied_request.find("X-Original-Host: example.com\r\n"), std::string::npos) << proxied_request;
}

} // namespace
