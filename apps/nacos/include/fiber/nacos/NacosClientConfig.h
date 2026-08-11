#ifndef FIBER_NACOS_NACOS_CLIENT_CONFIG_H
#define FIBER_NACOS_NACOS_CLIENT_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/net/IpAddress.h>

namespace fiber::nacos {

enum class NacosConfigErrorCode : std::uint8_t {
    EmptyServerList,
    InvalidServerHost,
    InvalidHttpPort,
    InvalidGrpcPort,
    EmptyUsername,
    EmptyPassword,
};

class NacosServerHost {
public:
    enum class Kind : std::uint8_t {
        IpLiteral,
        Hostname,
    };

    [[nodiscard]] static std::expected<NacosServerHost, NacosConfigErrorCode> create(std::string host);

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_ip_literal() const noexcept { return kind_ == Kind::IpLiteral; }
    [[nodiscard]] std::string_view value() const noexcept { return value_; }
    [[nodiscard]] const net::IpAddress &literal_ip() const noexcept { return literal_ip_; }

private:
    NacosServerHost(std::string value, net::IpAddress literal_ip, Kind kind) noexcept;

    std::string value_;
    net::IpAddress literal_ip_{};
    Kind kind_ = Kind::Hostname;
};

struct NacosClientConfigParams {
    std::vector<std::string> server_hosts;
    std::string username;
    std::string password;
    std::uint16_t http_port = 8848;
    std::uint16_t grpc_port = 9848;
    std::string namespace_id;
    std::string tenant;
    std::string client_version = "fiber-nacos/1.0";
};

struct NacosConfigError {
    NacosConfigErrorCode code = NacosConfigErrorCode::EmptyServerList;
    std::size_t server_index = 0;
};

class NacosClientConfig {
public:
    [[nodiscard]] static std::expected<NacosClientConfig, NacosConfigError> create(NacosClientConfigParams params);

    [[nodiscard]] const std::vector<NacosServerHost> &server_hosts() const noexcept { return server_hosts_; }
    [[nodiscard]] bool has_hostname_server() const noexcept { return has_hostname_server_; }
    [[nodiscard]] const std::string &username() const noexcept { return username_; }
    [[nodiscard]] const std::string &password() const noexcept { return password_; }
    [[nodiscard]] std::uint16_t http_port() const noexcept { return http_port_; }
    [[nodiscard]] std::uint16_t grpc_port() const noexcept { return grpc_port_; }
    [[nodiscard]] const std::string &namespace_id() const noexcept { return namespace_id_; }
    [[nodiscard]] const std::string &tenant() const noexcept { return tenant_; }
    [[nodiscard]] const std::string &client_version() const noexcept { return client_version_; }

private:
    NacosClientConfig(NacosClientConfigParams params, std::vector<NacosServerHost> server_hosts) noexcept;

    std::vector<NacosServerHost> server_hosts_;
    std::string username_;
    std::string password_;
    std::uint16_t http_port_ = 8848;
    std::uint16_t grpc_port_ = 9848;
    std::string namespace_id_;
    std::string tenant_;
    std::string client_version_;
    bool has_hostname_server_ = false;
};

struct NacosClientOptions {
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds request_timeout{3000};
    std::size_t max_auth_response_bytes = 64 * 1024;
    std::chrono::milliseconds retry_initial_delay{1000};
    std::chrono::milliseconds retry_max_delay{30000};
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_CLIENT_CONFIG_H
