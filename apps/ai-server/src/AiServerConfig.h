#ifndef FIBER_AI_SERVER_AI_SERVER_CONFIG_H
#define FIBER_AI_SERVER_AI_SERVER_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/cat/CatClientConfig.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <net/LocalAddress.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {

using LocalIpv4Detector = std::expected<net::LocalIpv4Selection, net::LocalIpv4Error> (*)() noexcept;

enum class AiServerConfigErrorCode : std::uint8_t {
    OpenFailed,
    ReadFailed,
    InvalidSyntax,
    DuplicateKey,
    UnknownKey,
    MissingRequiredKey,
    InvalidValue,
    InvalidNacosConfig,
    InvalidCatConfig,
    LocalAddressUnavailable,
};

struct AiServerConfigError {
    AiServerConfigErrorCode code = AiServerConfigErrorCode::InvalidSyntax;
    std::size_t line = 0;
    std::string key;
    std::string detail;
};

class AiServerConfig {
public:
    [[nodiscard]] static std::expected<AiServerConfig, AiServerConfigError>
    load_from_file(std::string_view path, LocalIpv4Detector detector = net::detect_local_ipv4);
    [[nodiscard]] static std::expected<AiServerConfig, AiServerConfigError>
    load_from_string(std::string_view input, LocalIpv4Detector detector = net::detect_local_ipv4);

    [[nodiscard]] const net::SocketAddress &listen_address() const noexcept { return listen_address_; }
    [[nodiscard]] const nacos::NacosClientConfig &nacos_config() const noexcept { return nacos_config_; }
    [[nodiscard]] std::chrono::milliseconds initial_config_timeout() const noexcept { return initial_config_timeout_; }
    [[nodiscard]] const net::IpAddress &advertise_address() const noexcept { return advertise_address_; }
    [[nodiscard]] const std::optional<net::LocalIpv4Selection> &detected_local_ipv4() const noexcept {
        return detected_local_ipv4_;
    }
    [[nodiscard]] std::string_view service_name() const noexcept { return service_name_; }
    [[nodiscard]] std::string_view service_group() const noexcept { return service_group_; }
    [[nodiscard]] std::string_view zone() const noexcept { return zone_; }
    [[nodiscard]] std::string_view cluster() const noexcept { return cluster_; }
    [[nodiscard]] std::string nacos_cluster() const;
    [[nodiscard]] const std::optional<cat::CatClientConfig> &cat_config() const noexcept { return cat_config_; }
    [[nodiscard]] std::string_view logging_config_path() const noexcept { return logging_config_path_; }

private:
    AiServerConfig(net::SocketAddress listen_address, nacos::NacosClientConfig nacos_config,
                   std::chrono::milliseconds initial_config_timeout, net::IpAddress advertise_address,
                   std::optional<net::LocalIpv4Selection> detected_local_ipv4, std::string service_name,
                   std::string service_group, std::string zone, std::string cluster,
                   std::optional<cat::CatClientConfig> cat_config, std::string logging_config_path) noexcept;

    net::SocketAddress listen_address_;
    nacos::NacosClientConfig nacos_config_;
    std::chrono::milliseconds initial_config_timeout_;
    net::IpAddress advertise_address_;
    std::optional<net::LocalIpv4Selection> detected_local_ipv4_;
    std::string service_name_;
    std::string service_group_;
    std::string zone_;
    std::string cluster_;
    std::optional<cat::CatClientConfig> cat_config_;
    std::string logging_config_path_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_CONFIG_H
