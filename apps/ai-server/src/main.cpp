#include "AiServer.h"
#include "AiServerConfig.h"

#include <csignal>
#include <cstdint>
#include <expected>
#include <iostream>
#include <string_view>
#include <utility>

#include <cerrno>
#include <sys/socket.h>

#include "async/Spawn.h"
#include "common/IoError.h"
#include "event/EventLoop.h"
#include "net/SocketAddress.h"
#include "net/TcpListener.h"

namespace {

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

void print_config_error(std::string_view path, const fiber::ai_server::AiServerConfigError &error) {
    std::cerr << "configuration error in " << path;
    if (error.line != 0) {
        std::cerr << ':' << error.line;
    }
    if (!error.key.empty()) {
        std::cerr << " [" << error.key << ']';
    }
    std::cerr << ": " << error.detail << '\n';
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 2) {
        std::cerr << "usage: ai-server [config.env]\n";
        return 1;
    }

    const std::string_view config_path = argc == 2 ? std::string_view(argv[1]) : std::string_view("ai-server.env");
    auto config_result = fiber::ai_server::AiServerConfig::load_from_file(config_path);
    if (!config_result) {
        print_config_error(config_path, config_result.error());
        return 1;
    }
    fiber::ai_server::AiServerConfig config = std::move(*config_result);

    (void) ::signal(SIGPIPE, SIG_IGN);

    fiber::event::EventLoop loop;
    fiber::ai_server::AiServer server(loop);

    fiber::net::ListenOptions listen_options{};
    auto bind_result = server.bind(config.listen_address(), listen_options);
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

    const fiber::net::SocketAddress bound_address(config.listen_address().ip(), *bound_port);
    std::cout << "ai-server listening on http://" << bound_address.to_string()
              << ", nacos servers=" << config.nacos_config().server_ips().size() << std::endl;
    fiber::async::spawn(loop, [&server]() { return server.serve(); });
    loop.run();
    return 0;
}
