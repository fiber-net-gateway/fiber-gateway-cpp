#ifndef FIBER_AI_SERVER_PROVIDER_ENDPOINT_H
#define FIBER_AI_SERVER_PROVIDER_ENDPOINT_H

#include <cstdint>
#include <expected>
#include <string_view>

#include <fiber/net/IpAddress.h>

namespace fiber::ai_server {

enum class ProviderEndpointScheme : std::uint8_t {
    Http,
    Https,
    Service,
};

enum class ProviderEndpointErrorCode : std::uint8_t {
    UnsupportedScheme,
    MissingHost,
    InvalidAuthority,
    InvalidPort,
    InvalidPath,
    InvalidServiceName,
};

struct ProviderEndpointError {
    ProviderEndpointErrorCode code = ProviderEndpointErrorCode::UnsupportedScheme;
    const char *message = nullptr;
};

struct ParsedProviderEndpoint {
    ProviderEndpointScheme scheme = ProviderEndpointScheme::Http;
    std::string_view host;
    std::string_view base_path;
    net::IpAddress ip;
    std::uint16_t port = 0;
    bool host_is_ip = false;

    [[nodiscard]] bool is_service() const noexcept { return scheme == ProviderEndpointScheme::Service; }
    [[nodiscard]] bool tls() const noexcept { return scheme == ProviderEndpointScheme::Https; }
};

[[nodiscard]] std::expected<ParsedProviderEndpoint, ProviderEndpointError>
parse_provider_endpoint(std::string_view base_url) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_ENDPOINT_H
