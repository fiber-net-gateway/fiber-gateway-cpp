#ifndef FIBER_NACOS_NACOS_RPC_OPTIONS_H
#define FIBER_NACOS_NACOS_RPC_OPTIONS_H

#include <chrono>
#include <cstddef>
#include <string>

#include <fiber/net/TcpSocketOptions.h>

namespace fiber::nacos {

struct NacosRpcOptions {
    std::chrono::milliseconds connect_timeout{3000};
    net::TcpSocketOptions tcp{.no_delay = net::TcpOptionMode::Enabled};
    std::chrono::milliseconds request_timeout{3000};
    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds compatibility_setup_delay{1000};
    std::chrono::milliseconds heartbeat_interval{10000};
    std::chrono::milliseconds reconnect_initial_delay{1000};
    std::chrono::milliseconds reconnect_max_delay{60000};
    std::size_t max_inbound_message_bytes = static_cast<std::size_t>(10 * 1024 * 1024);
    std::size_t max_push_response_bytes = static_cast<std::size_t>(1024 * 1024);
    std::string client_ip_override;
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_RPC_OPTIONS_H
