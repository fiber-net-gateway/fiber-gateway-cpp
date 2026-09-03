#include "http/TlsAlpn.h"

namespace fiber::http {

namespace {

constexpr std::string_view kHttp1AlpnList[] = {"http/1.1"};
constexpr std::string_view kHttpServerAlpnList[] = {"h2", "http/1.1"};
constexpr std::string_view kHttp3AlpnList[] = {"h3"};

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

net::TlsServerParam make_server_tls_param(const HttpServerTlsOptions &options,
                                          std::span<const std::string_view> alpn) noexcept {
    net::TlsServerParam param{};
    param.configure_callback = options.configure_callback;
    param.configure_ctx = options.configure_ctx;
    param.trust_store = options.trust_store;
    param.client_certificate_mode = options.client_certificate_mode;
    param.alpn = alpn;
    param.min_version = options.min_version;
    param.max_version = options.max_version;
    param.enable_early_data = options.enable_early_data;
    return param;
}

} // namespace

net::TlsClientParam make_http1_client_tls_param(const HttpClientTlsOptions &options) noexcept {
    return make_http_client_tls_param(options, "http/1.1");
}

net::TlsClientParam make_http2_client_tls_param(const HttpClientTlsOptions &options) noexcept {
    return make_http_client_tls_param(options, "h2");
}

net::TlsServerParam make_http1_server_tls_param(const HttpServerTlsOptions &options) noexcept {
    return make_server_tls_param(options, kHttp1AlpnList);
}

net::TlsServerParam make_http_server_tls_param(const HttpServerTlsOptions &options) noexcept {
    return make_server_tls_param(options, kHttpServerAlpnList);
}

net::TlsServerParam make_http3_server_tls_param(const HttpServerTlsOptions &options) noexcept {
    return make_server_tls_param(options, kHttp3AlpnList);
}

} // namespace fiber::http
