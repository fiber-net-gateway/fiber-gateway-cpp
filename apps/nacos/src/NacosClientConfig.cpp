#include <fiber/nacos/NacosClientConfig.h>

#include <algorithm>
#include <utility>

namespace fiber::nacos {
namespace {

bool invalid_context_path(const std::string &path) noexcept {
    if (path.empty() || path.front() != '/') {
        return true;
    }
    for (const char ch: path) {
        if (ch == '?' || ch == '#' || ch == '\r' || ch == '\n') {
            return true;
        }
    }
    return false;
}

} // namespace

NacosClientConfig::NacosClientConfig(NacosClientConfigParams params) noexcept :
    server_ips_(std::move(params.server_ips)), username_(std::move(params.username)),
    password_(std::move(params.password)), http_port_(params.http_port), grpc_port_(params.grpc_port),
    namespace_id_(std::move(params.namespace_id)), tenant_(std::move(params.tenant)),
    client_version_(std::move(params.client_version)), context_path_(std::move(params.context_path)) {}

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
    if (invalid_context_path(params.context_path)) {
        return std::unexpected(NacosConfigError{.code = NacosConfigErrorCode::InvalidContextPath});
    }

    while (params.context_path.size() > 1 && params.context_path.back() == '/') {
        params.context_path.pop_back();
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
