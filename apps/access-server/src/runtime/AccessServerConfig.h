#ifndef FIBER_ACCESS_SERVER_ACCESS_SERVER_CONFIG_H
#define FIBER_ACCESS_SERVER_ACCESS_SERVER_CONFIG_H

#include "AccessConfigWatcher.h"
#include "AccessServiceDiscovery.h"
#include "GrayConfigWatcher.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/cat/CatClientConfig.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <fiber/net/SocketAddress.h>

namespace fiber::access_server {

enum class AccessServerConfigErrorCode : std::uint8_t {
    OpenFailed,
    ReadFailed,
    InvalidSyntax,
    DuplicateKey,
    UnknownKey,
    MissingRequiredKey,
    InvalidValue,
    InvalidNacosConfig,
};

struct AccessServerConfigError {
    AccessServerConfigErrorCode code = AccessServerConfigErrorCode::InvalidSyntax;
    std::size_t line = 0;
    std::string key;
    std::string detail;
};

class AccessServerConfig {
public:
    [[nodiscard]] static std::expected<AccessServerConfig, AccessServerConfigError>
    load_from_file(std::string_view path);
    [[nodiscard]] static std::expected<AccessServerConfig, AccessServerConfigError>
    load_from_string(std::string_view input);

    [[nodiscard]] const net::SocketAddress &listen_address() const noexcept { return listen_address_; }
    [[nodiscard]] const net::SocketAddress &metrics_listen_address() const noexcept { return metrics_listen_address_; }
    [[nodiscard]] std::chrono::milliseconds initial_config_timeout() const noexcept { return initial_config_timeout_; }
    [[nodiscard]] std::size_t default_max_request_body_size() const noexcept { return default_max_request_body_size_; }
    [[nodiscard]] bool test_mode() const noexcept { return test_mode_; }
    [[nodiscard]] const std::optional<cat::CatClientConfig> &cat_config() const noexcept { return cat_config_; }
    [[nodiscard]] const nacos::NacosClientConfig &nacos_config() const noexcept { return nacos_config_; }
    [[nodiscard]] const AccessConfigWatcherOptions &watcher_options() const noexcept { return watcher_options_; }
    [[nodiscard]] const GrayConfigWatcherOptions &gray_watcher_options() const noexcept {
        return gray_watcher_options_;
    }
    [[nodiscard]] const AccessServiceDiscoveryOptions &service_discovery_options() const noexcept {
        return service_discovery_options_;
    }

private:
    AccessServerConfig(net::SocketAddress listen_address, net::SocketAddress metrics_listen_address,
                       std::chrono::milliseconds initial_config_timeout, std::size_t default_max_request_body_size,
                       bool test_mode, std::optional<cat::CatClientConfig> cat_config,
                       nacos::NacosClientConfig nacos_config, AccessConfigWatcherOptions watcher_options,
                       GrayConfigWatcherOptions gray_watcher_options,
                       AccessServiceDiscoveryOptions service_discovery_options) noexcept;

    net::SocketAddress listen_address_;
    net::SocketAddress metrics_listen_address_;
    std::chrono::milliseconds initial_config_timeout_{60000};
    std::size_t default_max_request_body_size_ = 400U << 20U;
    bool test_mode_ = false;
    std::optional<cat::CatClientConfig> cat_config_;
    nacos::NacosClientConfig nacos_config_;
    AccessConfigWatcherOptions watcher_options_;
    GrayConfigWatcherOptions gray_watcher_options_;
    AccessServiceDiscoveryOptions service_discovery_options_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_SERVER_CONFIG_H
