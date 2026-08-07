#include <fiber/http_script/HttpTarget.h>

#include <cctype>

namespace fiber::http_script {

namespace {

bool icase_starts_with(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

// Splits "host:port" / "[ipv6]:port" / "host" into host and port (0 when absent). Returns false if
// the authority contains a path/query/fragment (scripts must supply those via the path option).
bool split_authority(std::string_view auth, std::string &host, std::uint16_t &port) {
    if (auth.find('/') != std::string_view::npos || auth.find('?') != std::string_view::npos ||
        auth.find('#') != std::string_view::npos) {
        return false;
    }
    if (auth.empty()) {
        return false;
    }
    if (auth.front() == '[') {
        const std::size_t close = auth.find(']');
        if (close == std::string_view::npos || close + 1 >= auth.size() || auth[close + 1] != ':') {
            return false;
        }
        host.assign(auth.substr(1, close - 1));
        const std::string_view port_sv = auth.substr(close + 2);
        if (port_sv.empty()) {
            return false;
        }
        std::uint32_t p = 0;
        for (char ch: port_sv) {
            if (ch < '0' || ch > '9') {
                return false;
            }
            p = p * 10 + static_cast<std::uint32_t>(ch - '0');
            if (p > 65535) {
                return false;
            }
        }
        port = static_cast<std::uint16_t>(p);
        return true;
    }
    const std::size_t colon = auth.rfind(':');
    if (colon == std::string_view::npos) {
        host.assign(auth);
        port = 0;
        return true;
    }
    host.assign(auth.substr(0, colon));
    const std::string_view port_sv = auth.substr(colon + 1);
    if (port_sv.empty()) {
        return false;
    }
    std::uint32_t p = 0;
    for (char ch: port_sv) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        p = p * 10 + static_cast<std::uint32_t>(ch - '0');
        if (p > 65535) {
            return false;
        }
    }
    port = static_cast<std::uint16_t>(p);
    return true;
}

} // namespace

std::optional<HttpTargetSpec> HttpTargetSpec::parse(std::string_view literal) noexcept {
    if (literal.empty()) {
        return std::nullopt;
    }

    constexpr std::string_view kHttp = "http://";
    constexpr std::string_view kHttps = "https://";
    std::string_view authority;
    bool tls = false;
    if (icase_starts_with(literal, kHttps)) {
        authority = literal.substr(kHttps.size());
        tls = true;
    } else if (icase_starts_with(literal, kHttp)) {
        authority = literal.substr(kHttp.size());
        tls = false;
    } else {
        // Otherwise treat as an upstream name (with optional leading '@').
        HttpTargetSpec spec;
        spec.kind = HttpTargetSpec::Kind::Upstream;
        spec.name = std::string(literal);
        return spec;
    }

    HttpTargetSpec spec;
    spec.kind = HttpTargetSpec::Kind::Url;
    spec.tls = tls;
    if (!split_authority(authority, spec.name, spec.port)) {
        return std::nullopt;
    }
    return spec;
}

} // namespace fiber::http_script
