#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "config/ConfigLoader.h"
#include "event/EventLoop.h"
#include "runtime/RuntimeBuilder.h"
#include "runtime/ServerLauncher.h"

namespace {

struct ShutdownOp {
    fiber::event::EventLoop::NotifyEntry entry;
    fiber::lite_nginx::runtime::ServerLauncher *launcher = nullptr;
    fiber::event::EventLoop *loop = nullptr;

    static void on_run(ShutdownOp *self) {
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
)", "dup_server_name.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_FALSE(runtime.has_value());
    EXPECT_NE(runtime.error().message.find("duplicate server_name"), std::string::npos);
}

TEST(LiteNginxRuntimeTest, ServesHelloOnPlainHttp) {
    std::uint16_t port = reserve_loopback_port();
    ASSERT_NE(port, 0);

    std::string config_text = R"(
worker_processes 1;
http {
    listen 127.0.0.1:PORT;

    server {
        server_name localhost;
        location / {
            proxy_pass http://127.0.0.1:9001;
        }
    }
}
)";
    std::size_t marker = config_text.find("PORT");
    ASSERT_NE(marker, std::string::npos);
    config_text.replace(marker, 4, std::to_string(port));

    auto config = fiber::lite_nginx::config::ConfigLoader::load_from_string(config_text, "runtime_http.conf");
    ASSERT_TRUE(config.has_value());

    auto runtime = fiber::lite_nginx::runtime::RuntimeBuilder::build(*config);
    ASSERT_TRUE(runtime.has_value());

    fiber::event::EventLoop loop;
    fiber::lite_nginx::runtime::ServerLauncher launcher(loop);
    auto start_result = launcher.start(*runtime);
    ASSERT_TRUE(start_result.has_value()) << start_result.error().message;
    ASSERT_EQ(launcher.bound_listeners().size(), 1U);

    std::thread loop_thread([&loop]() {
        loop.run();
    });

    int client = connect_client(launcher.bound_listeners().front().address.port());
    ASSERT_GE(client, 0);

    const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_EQ(::send(client, request, sizeof(request) - 1, 0), static_cast<ssize_t>(sizeof(request) - 1));

    std::string response = recv_http_response(client);
    ::close(client);

    ShutdownOp shutdown{
        .launcher = &launcher,
        .loop = &loop,
    };
    loop.post<ShutdownOp, &ShutdownOp::entry, &ShutdownOp::on_run>(shutdown);
    loop_thread.join();

    EXPECT_NE(response.find("HTTP/1.1 200 OK\r\n"), std::string::npos);
    EXPECT_NE(response.find("hello lite nginx\n"), std::string::npos);
}

} // namespace
