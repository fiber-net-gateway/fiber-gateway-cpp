#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
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
        location / {
            proxy_pass http://127.0.0.1:9001;
        }
    }

    server {
        server_name same.test;
        location / {
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
        location / {
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
        location / {
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
        location / {
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
        keepalive 2;
    }

    server {
        server_name localhost;
        location / {
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
        location / {
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
        keepalive 2;
        keepalive_mode stealable;
    }

    server {
        server_name localhost;
        location / {
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
        location / {
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

TEST(LiteNginxRuntimeTest, ScriptFileLocationRejectsMissingFile) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server {
        server_name localhost;
        location / { script_file /tmp/lite_nginx_does_not_exist_999999.js; }
    }
}
)",
                                                                            "missing_script.conf");
    ASSERT_TRUE(config.has_value()) << config.error().message;

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("script_file not found"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ScriptFileAndProxyPassAreMutuallyExclusive) {
    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(R"(
worker_processes 1;
http {
    listen 127.0.0.1:8080;
    server {
        server_name localhost;
        location / {
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

} // namespace
