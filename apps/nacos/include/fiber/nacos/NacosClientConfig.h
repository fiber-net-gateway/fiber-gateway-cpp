#ifndef FIBER_NACOS_NACOS_CLIENT_CONFIG_H
#define FIBER_NACOS_NACOS_CLIENT_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <net/IpAddress.h>

namespace fiber::nacos {

enum class NacosAuthApiVersion : std::uint8_t {
    Auto,
    V3,
    LegacyV1,
};

struct NacosClientConfigParams {
    std::vector<net::IpAddress> server_ips;
    std::string username;
    std::string password;
    std::uint16_t http_port = 8848;
    std::uint16_t grpc_port = 9848;
    std::string namespace_id;
    std::string tenant;
    std::string client_version = "fiber-nacos/1.0";
    std::string context_path = "/nacos";
    NacosAuthApiVersion auth_api_version = NacosAuthApiVersion::Auto;
};

enum class NacosConfigErrorCode : std::uint8_t {
    EmptyServerList,
    InvalidServerAddress,
    InvalidHttpPort,
    InvalidGrpcPort,
    EmptyUsername,
    EmptyPassword,
    InvalidContextPath,
};

struct NacosConfigError {
    NacosConfigErrorCode code = NacosConfigErrorCode::EmptyServerList;
    std::size_t server_index = 0;
};

class NacosClientConfig {
public:
    [[nodiscard]] static std::expected<NacosClientConfig, NacosConfigError> create(NacosClientConfigParams params);

    [[nodiscard]] const std::vector<net::IpAddress> &server_ips() const noexcept { return server_ips_; }
    [[nodiscard]] const std::string &username() const noexcept { return username_; }
    [[nodiscard]] const std::string &password() const noexcept { return password_; }
    [[nodiscard]] std::uint16_t http_port() const noexcept { return http_port_; }
    [[nodiscard]] std::uint16_t grpc_port() const noexcept { return grpc_port_; }
    [[nodiscard]] const std::string &namespace_id() const noexcept { return namespace_id_; }
    [[nodiscard]] const std::string &tenant() const noexcept { return tenant_; }
    [[nodiscard]] const std::string &client_version() const noexcept { return client_version_; }
    [[nodiscard]] const std::string &context_path() const noexcept { return context_path_; }
    [[nodiscard]] NacosAuthApiVersion auth_api_version() const noexcept { return auth_api_version_; }

private:
    explicit NacosClientConfig(NacosClientConfigParams params) noexcept;

    std::vector<net::IpAddress> server_ips_;
    std::string username_;
    std::string password_;
    std::uint16_t http_port_ = 8848;
    std::uint16_t grpc_port_ = 9848;
    std::string namespace_id_;
    std::string tenant_;
    std::string client_version_;
    std::string context_path_;
    NacosAuthApiVersion auth_api_version_ = NacosAuthApiVersion::Auto;
};

struct NacosClientOptions {
    std::chrono::milliseconds connect_timeout{3000};
    std::chrono::milliseconds request_timeout{3000};
    std::size_t max_auth_response_bytes = 64 * 1024;
    std::chrono::milliseconds retry_initial_delay{1000};
    std::chrono::milliseconds retry_max_delay{30000};
    std::uint8_t refresh_percent = 90;
    std::chrono::milliseconds min_refresh_delay{1000};
};

} // namespace fiber::nacos

#endif // FIBER_NACOS_NACOS_CLIENT_CONFIG_H
