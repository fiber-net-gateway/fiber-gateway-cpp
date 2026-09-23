#include "TlsServerHandshakeShared.h"

#include <fiber/tls/handshake/TlsCipherSuites.h>

namespace fiber::tls {

bool tls_server_list_contains(std::span<const std::uint8_t> raw_list, std::uint16_t value) noexcept {
    if ((raw_list.size() & 1) != 0) {
        return false; // the CH decode guarantees even; defensive on rescans
    }
    for (std::size_t i = 0; i + 1 < raw_list.size(); i += 2) {
        const std::uint16_t entry =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(raw_list[i]) << 8) | raw_list[i + 1]);
        if (entry == value) {
            return true;
        }
    }
    return false;
}

bool tls_server_suite_select(const TlsClientHello &ch, bool tls13, TlsCipherSuiteId &out) noexcept {
    for (const std::uint16_t raw: kServerSuites) {
        if (!tls_server_list_contains(ch.cipher_suites, raw)) {
            continue;
        }
        const auto suite = static_cast<TlsCipherSuiteId>(raw);
        const TlsSuiteInfo *info = tls_suite_info(suite);
        if (info == nullptr || info->is_tls13 != tls13) {
            continue;
        }
        out = suite;
        return true;
    }
    return false;
}

namespace {

// Which credential kind signs a 1.2 registry suite (ECDHE-RSA vs
// ECDHE-ECDSA — the auth half of the suite name).
constexpr TlsKeyKind suite_auth(TlsCipherSuiteId suite) noexcept {
    switch (suite) {
        case TlsCipherSuiteId::EcdheRsaAes128GcmSha256:
        case TlsCipherSuiteId::EcdheRsaAes256GcmSha384:
        case TlsCipherSuiteId::EcdheRsaChacha20Poly1305:
            return TlsKeyKind::Rsa;
        default:
            return TlsKeyKind::Ec; // the ECDHE-ECDSA half of the registry
    }
}

} // namespace

bool tls_server_suite_select_12(const TlsClientHello &ch, const TlsPrivateKey &key, TlsCipherSuiteId &out) noexcept {
    const TlsKeyKind auth = key.key_kind();
    if (auth == TlsKeyKind::Ed25519) {
        return false; // no 1.2 registry entry signs with Ed25519
    }
    for (const std::uint16_t raw: kServerSuites) {
        if (!tls_server_list_contains(ch.cipher_suites, raw)) {
            continue;
        }
        const auto suite = static_cast<TlsCipherSuiteId>(raw);
        const TlsSuiteInfo *info = tls_suite_info(suite);
        if (info == nullptr || info->is_tls13 || suite_auth(suite) != auth) {
            continue;
        }
        out = suite;
        return true;
    }
    return false;
}

bool tls_server_group_select(const TlsClientHello &ch, TlsNamedGroup &out) noexcept {
    for (const std::uint16_t raw: kServerGroups) {
        if (tls_server_list_contains(ch.supported_groups, raw)) {
            out = static_cast<TlsNamedGroup>(raw);
            return true;
        }
    }
    return false;
}

TlsServerAlpnResult tls_server_alpn_select(const TlsServerConfig &cfg, const TlsClientHello &ch,
                                           std::string_view &out) noexcept {
    if (cfg.alpn.empty() || !ch.has_alpn) {
        return TlsServerAlpnResult::None;
    }
    // Server preference order (RFC 7301 §3.2: the server picks).
    for (const std::string_view want: cfg.alpn) {
        if (want.empty()) {
            continue;
        }
        // Walk the client's ProtocolNameList for this protocol.
        const std::uint8_t *p = ch.alpn_list.data();
        const std::uint8_t *const end = p + ch.alpn_list.size();
        while (p < end) {
            const std::uint16_t name_len = *p++;
            if (static_cast<std::size_t>(name_len) > static_cast<std::size_t>(end - p)) {
                return TlsServerAlpnResult::None; // malformed list — treat as not offered
            }
            if (name_len == want.size() && std::string_view{reinterpret_cast<const char *>(p), name_len} == want) {
                out = want;
                return TlsServerAlpnResult::Matched;
            }
            p += name_len;
        }
    }
    return TlsServerAlpnResult::Failed;
}

bool tls_server_cv_scheme_select(const TlsPrivateKey &key, const TlsClientHello &ch, TlsProtocolVersion version,
                                 TlsSignatureScheme &out) noexcept {
    if (!ch.has_signature_algorithms) {
        return false; // RFC 8446 §4.2.3: mandatory in a 1.3 CH
    }
    if (version == TlsProtocolVersion::Tls13) {
        for (const TlsSignatureScheme scheme: kTls13SignaturePreference) {
            if (key.supports(scheme, version) &&
                tls_server_list_contains(ch.signature_algorithms, static_cast<std::uint16_t>(scheme))) {
                out = scheme;
                return true;
            }
        }
        return false;
    }
    for (const TlsSignatureScheme scheme: kTls12SignaturePreference) {
        if (key.supports(scheme, version) &&
            tls_server_list_contains(ch.signature_algorithms, static_cast<std::uint16_t>(scheme))) {
            out = scheme;
            return true;
        }
    }
    return false;
}

} // namespace fiber::tls
