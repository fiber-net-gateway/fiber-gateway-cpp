#include "http/TlsAlpn.h"

#include <algorithm>
#include <string>
#include <vector>

namespace fiber::http {

namespace {

template<typename Options>
std::vector<std::string> normalize_base(const Options &options) {
    std::vector<std::string> normalized;
    normalized.reserve(options.alpn.size() + 2);
    for (const auto &proto: options.alpn) {
        if (proto.empty()) {
            continue;
        }
        if (std::find(normalized.begin(), normalized.end(), proto) != normalized.end()) {
            continue;
        }
        normalized.push_back(proto);
    }
    return normalized;
}

template<typename Options>
void normalize_http1_alpn_impl(Options &options) {
    std::vector<std::string> normalized = normalize_base(options);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "http/1.1");
    options.alpn = std::move(normalized);
}

template<typename Options>
void normalize_http3_alpn_impl(Options &options) {
    std::vector<std::string> normalized = normalize_base(options);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h3"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "h3");
    options.alpn = std::move(normalized);
}

} // namespace

void normalize_http1_alpn(net::TlsClientParam &options) { normalize_http1_alpn_impl(options); }

void normalize_http1_alpn(net::TlsServerParam &options) { normalize_http1_alpn_impl(options); }

void normalize_http_server_alpn(net::TlsServerParam &options) {
    std::vector<std::string> normalized = normalize_base(options);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "http/1.1");
    normalized.insert(normalized.begin(), "h2");
    options.alpn = std::move(normalized);
}

void normalize_http3_alpn(net::TlsClientParam &options) { normalize_http3_alpn_impl(options); }

void normalize_http3_alpn(net::TlsServerParam &options) { normalize_http3_alpn_impl(options); }

} // namespace fiber::http
