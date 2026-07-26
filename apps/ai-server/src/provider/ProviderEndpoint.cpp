#include "ProviderEndpoint.h"

#include <cstddef>
#include <limits>

namespace fiber::ai_server {
namespace {

ProviderEndpointError error(ProviderEndpointErrorCode code, const char *message) noexcept {
    return ProviderEndpointError{.code = code, .message = message};
}

bool parse_port(std::string_view text, std::uint16_t &port) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint32_t value = 0;
    for (const unsigned char ch: text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint32_t>(ch - '0');
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            return false;
        }
    }
    if (value == 0) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool valid_service_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 1024) {
        return false;
    }
    for (const unsigned char ch: name) {
        if (ch <= 0x20 || ch == '/' || ch == '?' || ch == '#') {
            return false;
        }
    }
    return true;
}

bool valid_host_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    for (const unsigned char ch: name) {
        if (ch <= 0x20 || ch >= 0x7f || ch == '/' || ch == '\\' || ch == ':' || ch == '?' || ch == '#') {
            return false;
        }
    }
    return true;
}

} // namespace

std::expected<ParsedProviderEndpoint, ProviderEndpointError>
parse_provider_endpoint(std::string_view base_url) noexcept {
    ParsedProviderEndpoint endpoint;
    std::string_view remainder;
    if (base_url.starts_with("http://")) {
        endpoint.scheme = ProviderEndpointScheme::Http;
        endpoint.port = 80;
        remainder = base_url.substr(7);
    } else if (base_url.starts_with("https://")) {
        endpoint.scheme = ProviderEndpointScheme::Https;
        endpoint.port = 443;
        remainder = base_url.substr(8);
    } else if (base_url.starts_with("service://")) {
        endpoint.scheme = ProviderEndpointScheme::Service;
        endpoint.host = base_url.substr(10);
        if (!valid_service_name(endpoint.host)) {
            return std::unexpected(error(ProviderEndpointErrorCode::InvalidServiceName, "invalid service name"));
        }
        return endpoint;
    } else {
        return std::unexpected(error(ProviderEndpointErrorCode::UnsupportedScheme, "unsupported provider URL scheme"));
    }

    const std::size_t path_pos = remainder.find('/');
    const std::string_view authority = path_pos == std::string_view::npos ? remainder : remainder.substr(0, path_pos);
    endpoint.base_path = path_pos == std::string_view::npos ? std::string_view{} : remainder.substr(path_pos);
    if (authority.empty()) {
        return std::unexpected(error(ProviderEndpointErrorCode::MissingHost, "provider URL host is required"));
    }
    if (authority.find('@') != std::string_view::npos || authority.find('?') != std::string_view::npos ||
        authority.find('#') != std::string_view::npos) {
        return std::unexpected(error(ProviderEndpointErrorCode::InvalidAuthority, "invalid provider URL authority"));
    }
    if (endpoint.base_path.find('?') != std::string_view::npos ||
        endpoint.base_path.find('#') != std::string_view::npos ||
        endpoint.base_path.find('\\') != std::string_view::npos) {
        return std::unexpected(error(ProviderEndpointErrorCode::InvalidPath, "invalid provider base path"));
    }

    std::string_view port_text;
    bool explicit_port = false;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string_view::npos || close == 1) {
            return std::unexpected(error(ProviderEndpointErrorCode::InvalidAuthority, "invalid IPv6 authority"));
        }
        endpoint.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                return std::unexpected(error(ProviderEndpointErrorCode::InvalidAuthority, "invalid IPv6 authority"));
            }
            explicit_port = true;
            port_text = authority.substr(close + 2);
        }
        if (!net::IpAddress::parse(endpoint.host, endpoint.ip) || !endpoint.ip.is_v6()) {
            return std::unexpected(error(ProviderEndpointErrorCode::InvalidAuthority, "invalid IPv6 address"));
        }
        endpoint.host_is_ip = true;
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            if (authority.find(':') != colon) {
                return std::unexpected(
                        error(ProviderEndpointErrorCode::InvalidAuthority, "IPv6 addresses must use brackets"));
            }
            endpoint.host = authority.substr(0, colon);
            explicit_port = true;
            port_text = authority.substr(colon + 1);
        } else {
            endpoint.host = authority;
        }
        if (!valid_host_name(endpoint.host)) {
            return std::unexpected(error(ProviderEndpointErrorCode::MissingHost, "invalid provider URL host"));
        }
        endpoint.host_is_ip = net::IpAddress::parse(endpoint.host, endpoint.ip);
    }

    if (explicit_port && !parse_port(port_text, endpoint.port)) {
        return std::unexpected(error(ProviderEndpointErrorCode::InvalidPort, "invalid provider URL port"));
    }
    return endpoint;
}

} // namespace fiber::ai_server
