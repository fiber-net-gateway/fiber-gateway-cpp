#include "http/TlsAlpn.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fiber::http {

namespace {

net::TlsClientParam make_http_client_tls_param(const HttpClientTlsOptions &options, const char *alpn) noexcept {
    net::TlsClientParam param{};
    param.security = options.security;
    param.min_version = options.min_version;
    param.max_version = options.max_version;
    param.alpn.emplace_back(alpn);
    param.server_name = options.server_name;
    param.verify_name = options.verify_name;
    return param;
}

std::vector<std::string> normalize_base(std::span<const std::string_view> alpn) {
    std::vector<std::string> normalized;
    normalized.reserve(alpn.size() + 2);
    for (const auto &proto: alpn) {
        if (proto.empty()) {
            continue;
        }
        if (std::find(normalized.begin(), normalized.end(), proto) != normalized.end()) {
            continue;
        }
        normalized.emplace_back(proto);
    }
    return normalized;
}

void rebind(net::TlsServerParam &options, net::TlsAlpnList &alpn, std::vector<std::string> normalized) {
    alpn.assign(std::move(normalized));
    options.alpn = alpn.view();
}

} // namespace

net::TlsClientParam make_http1_client_tls_param(const HttpClientTlsOptions &options) noexcept {
    return make_http_client_tls_param(options, "http/1.1");
}

net::TlsClientParam make_http2_client_tls_param(const HttpClientTlsOptions &options) noexcept {
    return make_http_client_tls_param(options, "h2");
}

void normalize_http1_alpn(net::TlsServerParam &options, net::TlsAlpnList &alpn) {
    std::vector<std::string> normalized = normalize_base(options.alpn);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "http/1.1");
    rebind(options, alpn, std::move(normalized));
}

void normalize_http_server_alpn(net::TlsServerParam &options, net::TlsAlpnList &alpn) {
    std::vector<std::string> normalized = normalize_base(options.alpn);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "http/1.1");
    normalized.insert(normalized.begin(), "h2");
    rebind(options, alpn, std::move(normalized));
}

void normalize_http3_alpn(net::TlsServerParam &options, net::TlsAlpnList &alpn) {
    std::vector<std::string> normalized = normalize_base(options.alpn);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h3"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "h3");
    rebind(options, alpn, std::move(normalized));
}

} // namespace fiber::http
