#include <fiber/net/detail/TlsClientStaging.h>

#include <chrono>
#include <cstring>

#include <fiber/net/IpAddress.h>
#include <fiber/net/TlsCredential.h>
#include <fiber/net/TrustStore.h>

namespace fiber::net::detail {

std::int64_t system_now_unix_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
}

common::IoResult<void> TlsClientStager::stage(const TlsClientParam &param, tls::TlsClientConfig &cfg,
                                              std::array<std::uint8_t, 16> &ip_bytes) noexcept {
    IpAddress server_ip{};
    const bool server_name_is_ip = !param.server_name.empty() && IpAddress::parse(param.server_name, server_ip);
    if (!param.server_name.empty() && !server_name_is_ip) {
        cfg.sni_host = param.server_name;
    }
    if (param.security.verify_peer) {
        // A null trust store means the process-wide system roots. Resolution
        // happens once per process; NotFound reports that no system CA bundle
        // exists on this host.
        const TrustStore *trust_store = param.security.trust_store;
        if (trust_store == nullptr) {
            auto system_store = TrustStore::system_default();
            if (!system_store) {
                return std::unexpected(system_store.error());
            }
            trust_store = *system_store;
        }
        cfg.trust = &trust_store->tls_store();
        const std::string_view verify_name = param.verify_name.empty() ? param.server_name : param.verify_name;
        IpAddress verify_ip{};
        if (!verify_name.empty() && IpAddress::parse(verify_name, verify_ip)) {
            std::memcpy(ip_bytes.data(), verify_ip.data(), verify_ip.byte_size());
            cfg.verify_ip = {ip_bytes.data(), verify_ip.byte_size()};
        } else if (!verify_name.empty()) {
            cfg.check_host = verify_name;
        } else {
            return std::unexpected(common::IoErr::Invalid);
        }
    } else {
        cfg.verify_peer = false;
    }
    if (param.security.credential != nullptr) {
        cfg.client_chain = &param.security.credential->tls_chain();
        cfg.client_key = &param.security.credential->tls_key();
    }
    cfg.alpn = param.alpn;
    cfg.now_unix_ms = system_now_unix_ms();
    return {};
}

} // namespace fiber::net::detail
