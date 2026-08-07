#include <fiber/http/TlsAlpn.h>

#include <string>
#include <vector>

namespace fiber::http {

namespace {

std::vector<std::string> normalize_base(const net::TlsOptions &options) {
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

} // namespace

void normalize_http1_alpn(net::TlsOptions &options) {
    std::vector<std::string> normalized = normalize_base(options);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "http/1.1");
    options.alpn = std::move(normalized);
}

void normalize_http_server_alpn(net::TlsOptions &options) {
    std::vector<std::string> normalized = normalize_base(options);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "http/1.1");
    normalized.insert(normalized.begin(), "h2");
    options.alpn = std::move(normalized);
}

void normalize_http3_alpn(net::TlsOptions &options) {
    std::vector<std::string> normalized = normalize_base(options);
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h3"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "h2"), normalized.end());
    normalized.erase(std::remove(normalized.begin(), normalized.end(), "http/1.1"), normalized.end());
    normalized.insert(normalized.begin(), "h3");
    options.alpn = std::move(normalized);
}

} // namespace fiber::http
