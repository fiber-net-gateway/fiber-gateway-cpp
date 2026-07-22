#include "AiServer.h"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <expected>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>

#include <cerrno>
#include <sys/socket.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace {

std::optional<std::uint16_t> parse_port(std::string_view text) noexcept {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

fiber::common::IoResult<std::uint16_t> resolve_port(int fd) noexcept {
    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &length) != 0) {
        return std::unexpected(fiber::common::io_err_from_errno(errno));
    }

    fiber::net::SocketAddress address;
    if (!fiber::net::SocketAddress::from_sockaddr(reinterpret_cast<sockaddr *>(&bound), length, address)) {
        return std::unexpected(fiber::common::IoErr::NotSupported);
    }
    return address.port();
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "usage: ai-server [port]\n";
        return 1;
    }

    std::uint16_t port = 8080;
    if (argc == 2) {
        auto parsed = parse_port(argv[1]);
        if (!parsed) {
            std::cerr << "usage: ai-server [port]\n";
            return 1;
        }
        port = *parsed;
    }

    (void) ::signal(SIGPIPE, SIG_IGN);

    fiber::event::EventLoop loop;
    fiber::ai_server::AiServer server(loop);

    fiber::net::ListenOptions listen_options{};
    auto bind_result = server.bind(fiber::net::SocketAddress::any_v4(port), listen_options);
    if (!bind_result) {
        std::cerr << "bind failed: " << fiber::common::io_err_name(bind_result.error()) << '\n';
        return 1;
    }

    auto bound_port = resolve_port(server.fd());
    if (!bound_port) {
        std::cerr << "resolve bound port failed: " << fiber::common::io_err_name(bound_port.error()) << '\n';
        server.close();
        return 1;
    }

    std::cout << "ai-server listening on http://0.0.0.0:" << *bound_port << std::endl;
    fiber::async::spawn(loop, [&server]() { return server.serve(); });
    loop.run();
    return 0;
}
