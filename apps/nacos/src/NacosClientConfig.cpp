#include <fiber/nacos/NacosClientConfig.h>

#include <algorithm>
#include <utility>

namespace fiber::nacos {

NacosClientConfig::NacosClientConfig(NacosClientConfigParams params) noexcept :
    server_ips_(std::move(params.server_ips)), username_(std::move(params.username)),
    password_(std::move(params.password)), http_port_(params.http_port), grpc_port_(params.grpc_port),
    namespace_id_(std::move(params.namespace_id)), tenant_(std::move(params.tenant)),
    client_version_(std::move(params.client_version)) {}

std::expected<NacosClientConfig, NacosConfigError> NacosClientConfig::create(NacosClientConfigParams params) {
    if (params.server_ips.empty()) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::EmptyServerList});
    }
    for (std::size_t i = 0; i < params.server_ips.size(); ++i) {
        if (params.server_ips[i].is_unspecified() || params.server_ips[i].is_multicast()) {
            return std::unexpected(
                    NacosConfigError{.code = NacosConfigErrorCode::InvalidServerAddress, .server_index = i});
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

    std::vector<net::IpAddress> unique_ips;
    unique_ips.reserve(params.server_ips.size());
    for (const net::IpAddress &ip: params.server_ips) {
        if (std::find(unique_ips.begin(), unique_ips.end(), ip) == unique_ips.end()) {
            unique_ips.push_back(ip);
        }
    }
    params.server_ips = std::move(unique_ips);
    return NacosClientConfig(std::move(params));
}

} // namespace fiber::nacos
