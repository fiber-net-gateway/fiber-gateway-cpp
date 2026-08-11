#include <fiber/nacos/NacosClientConfig.h>

#include <algorithm>
#include <utility>

namespace fiber::nacos {
namespace {

bool is_ascii_alnum(unsigned char ch) noexcept {
    return (ch >= static_cast<unsigned char>('a') && ch <= static_cast<unsigned char>('z')) ||
           (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) ||
           (ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9'));
}

bool valid_hostname(std::string_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.') {
        return false;
    }

    std::size_t label_size = 0;
    bool label_starts_with_hyphen = false;
    for (std::size_t i = 0; i < host.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(host[i]);
        if (ch == static_cast<unsigned char>('.')) {
            if (label_size == 0 || label_size > 63 || label_starts_with_hyphen || host[i - 1] == '-') {
                return false;
            }
            label_size = 0;
            label_starts_with_hyphen = false;
            continue;
        }
        if (!is_ascii_alnum(ch) && ch != static_cast<unsigned char>('-') && ch != static_cast<unsigned char>('_')) {
            return false;
        }
        if (label_size == 0) {
            label_starts_with_hyphen = ch == static_cast<unsigned char>('-');
        }
        ++label_size;
    }
    return label_size != 0 && label_size <= 63 && !label_starts_with_hyphen && host.back() != '-';
}

} // namespace

NacosServerHost::NacosServerHost(std::string value, net::IpAddress literal_ip, Kind kind) noexcept :
    value_(std::move(value)), literal_ip_(literal_ip), kind_(kind) {}

std::expected<NacosServerHost, NacosConfigErrorCode> NacosServerHost::create(std::string host) {
    if (host.empty()) {
        return std::unexpected(NacosConfigErrorCode::InvalidServerHost);
    }

    net::IpAddress literal;
    if (net::IpAddress::parse(host, literal)) {
        if (literal.is_unspecified() || literal.is_multicast()) {
            return std::unexpected(NacosConfigErrorCode::InvalidServerHost);
        }
        return NacosServerHost(literal.to_string(), literal, Kind::IpLiteral);
    }

    if (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (!valid_hostname(host)) {
        return std::unexpected(NacosConfigErrorCode::InvalidServerHost);
    }
    for (char &ch: host) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return NacosServerHost(std::move(host), {}, Kind::Hostname);
}

NacosClientConfig::NacosClientConfig(NacosClientConfigParams params, std::vector<NacosServerHost> server_hosts) noexcept
    :
    server_hosts_(std::move(server_hosts)), username_(std::move(params.username)),
    password_(std::move(params.password)), http_port_(params.http_port), grpc_port_(params.grpc_port),
    namespace_id_(std::move(params.namespace_id)), tenant_(std::move(params.tenant)),
    client_version_(std::move(params.client_version)) {
    for (const NacosServerHost &host: server_hosts_) {
        has_hostname_server_ = has_hostname_server_ || !host.is_ip_literal();
    }
}

std::expected<NacosClientConfig, NacosConfigError> NacosClientConfig::create(NacosClientConfigParams params) {
    if (params.server_hosts.empty()) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::EmptyServerList});
    }
    std::vector<NacosServerHost> unique_hosts;
    unique_hosts.reserve(params.server_hosts.size());
    for (std::size_t i = 0; i < params.server_hosts.size(); ++i) {
        auto host = NacosServerHost::create(std::move(params.server_hosts[i]));
        if (!host) {
            return std::unexpected(
                    NacosConfigError{.code = NacosConfigErrorCode::InvalidServerHost, .server_index = i});
        }
        const auto duplicate = std::find_if(unique_hosts.begin(), unique_hosts.end(), [&host](const auto &candidate) {
            return candidate.value() == host->value();
        });
        if (duplicate == unique_hosts.end()) {
            unique_hosts.push_back(std::move(*host));
        }
    }
    if (params.http_port == 0) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::InvalidHttpPort});
    }
    if (params.grpc_port == 0) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::InvalidGrpcPort});
    }
    if (params.username.empty() && !params.password.empty()) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::EmptyUsername});
    }
    if (!params.username.empty() && params.password.empty()) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::EmptyPassword});
    }

    return NacosClientConfig(std::move(params), std::move(unique_hosts));
}

} // namespace fiber::nacos
