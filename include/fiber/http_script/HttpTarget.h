#ifndef FIBER_HTTP_SCRIPT_HTTP_TARGET_H
#define FIBER_HTTP_SCRIPT_HTTP_TARGET_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fiber::http_script {

// Describes where an svc.request / svc.proxyPass call should send its upstream request. This is
// the compile-time target of a `directive <name> = http "<target>";` binding -- it is NOT the
// `url` option field of a call (that field is the request path?query).
//   - Upstream: a named upstream block ("@backend" or "backend"). Peer selection + pooling are
//     driven by UpstreamRegistry (weighted round-robin, connection_key from config-time IP).
//   - Url: an ad-hoc "http(s)://host[:port]" target. host may be an IP literal (no DNS) or a
//     hostname (resolved via the per-loop DnsResolver at call time). Connections pool under the
//     same global pool, keyed by the resolved peer identity.
struct HttpTargetSpec {
    enum class Kind : std::uint8_t {
        Upstream,
        Url,
    };

    Kind kind = Kind::Upstream;
    std::string name; // Upstream: upstream name (leading '@' optional); Url: host
    std::uint16_t port = 0; // Url only; 0 means scheme default (80/443)
    bool tls = false; // Url only

    [[nodiscard]] static std::optional<HttpTargetSpec> parse(std::string_view literal) noexcept;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_HTTP_TARGET_H
