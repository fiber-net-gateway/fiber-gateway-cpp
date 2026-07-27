#ifndef FIBER_AI_SERVER_AI_SERVER_CONFIG_H
#define FIBER_AI_SERVER_AI_SERVER_CONFIG_H

#include "audit/LlmAuditWriter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <fiber/cat/CatClientConfig.h>
#include <fiber/nacos/NacosClientConfig.h>
#include <net/SocketAddress.h>

namespace fiber::ai_server {

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
};

struct AiServerConfigError {
    AiServerConfigErrorCode code = AiServerConfigErrorCode::InvalidSyntax;
    std::size_t line = 0;
    std::string key;
    std::string detail;
};

class AiServerConfig {
public:
    [[nodiscard]] static std::expected<AiServerConfig, AiServerConfigError> load_from_file(std::string_view path);
    [[nodiscard]] static std::expected<AiServerConfig, AiServerConfigError> load_from_string(std::string_view input);

    [[nodiscard]] const net::SocketAddress &listen_address() const noexcept { return listen_address_; }
    [[nodiscard]] const nacos::NacosClientConfig &nacos_config() const noexcept { return nacos_config_; }
    [[nodiscard]] std::chrono::milliseconds initial_config_timeout() const noexcept { return initial_config_timeout_; }
    [[nodiscard]] const std::optional<net::IpAddress> &advertise_address() const noexcept { return advertise_address_; }
    [[nodiscard]] std::string_view service_name() const noexcept { return service_name_; }
    [[nodiscard]] std::string_view service_group() const noexcept { return service_group_; }
    [[nodiscard]] const std::optional<cat::CatClientConfig> &cat_config() const noexcept { return cat_config_; }
    [[nodiscard]] const LlmAuditWriterOptions &audit_writer_options() const noexcept { return audit_writer_options_; }

private:
    AiServerConfig(net::SocketAddress listen_address, nacos::NacosClientConfig nacos_config,
                   std::chrono::milliseconds initial_config_timeout, std::optional<net::IpAddress> advertise_address,
                   std::string service_name, std::string service_group, std::optional<cat::CatClientConfig> cat_config,
                   LlmAuditWriterOptions audit_writer_options) noexcept;

    net::SocketAddress listen_address_;
    nacos::NacosClientConfig nacos_config_;
    std::chrono::milliseconds initial_config_timeout_;
    std::optional<net::IpAddress> advertise_address_;
    std::string service_name_;
    std::string service_group_;
    std::optional<cat::CatClientConfig> cat_config_;
    LlmAuditWriterOptions audit_writer_options_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_AI_SERVER_CONFIG_H
