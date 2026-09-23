#include "TlsClientHandshakeShared.h"

#include <array>
#include <utility>

#include "../crypto/TlsCryptoPrimitives.h"

#include <fiber/tls/handshake/TlsHandshakeCodec.h>

namespace fiber::tls {

common::IoResult<std::unique_ptr<TlsKeyExchange>> tls_client_kx_offer(TlsNamedGroup group) noexcept {
    auto kx = TlsKeyExchange::create(group);
    if (!kx.has_value()) {
        return kx;
    }
    auto generated = (*kx)->generate();
    if (!generated.has_value()) {
        return std::unexpected(generated.error());
    }
    return kx;
}

bool tls_client_hello_build(TlsClientHelloState &hello, const TlsClientConfig &cfg, const TlsSessionOffer *session,
                            bool psk_offered, bool early_data_ext, bool second, std::uint16_t share_group,
                            std::span<const std::uint8_t> cookie) noexcept {
    if (!second) {
        // CH1 draws fresh entropy; CH2 keeps CH1's random and session_id
        // (RFC 8446 §4.1.4 — only key_share/cookie change).
        if (!tls_random_bytes(hello.client_random) || !tls_random_bytes(hello.session_id)) {
            return false;
        }
    }

    auto kx = tls_client_kx_offer(static_cast<TlsNamedGroup>(share_group));
    if (!kx.has_value()) {
        return false;
    }
    hello.kx = std::move(*kx);
    hello.kx_group = static_cast<TlsNamedGroup>(share_group);

    TlsClientHelloInput in{};
    in.random = hello.client_random;
    in.session_id = hello.session_id;
    in.cipher_suites = kOfferedSuites;
    in.supported_groups = kOfferedGroups;
    in.signature_algorithms = kOfferedSigalgs;
    // supported_versions narrows to the config bounds (09 §4.2); the domain
    // is {1.2, 1.3} with 1.3 the ceiling, so the list is 0-2 entries,
    // descending. An empty window (min > max) fails the encode below.
    std::array<std::uint16_t, 2> versions{};
    std::size_t version_count = 0;
    if (cfg.max_version >= kTlsVersionTls13 && cfg.min_version <= kTlsVersionTls13) {
        versions[version_count++] = kTlsVersionTls13;
    }
    if (cfg.max_version >= kTlsVersionTls12 && cfg.min_version <= kTlsVersionTls12) {
        versions[version_count++] = kTlsVersionTls12;
    }
    if (version_count == 0) {
        return false; // empty window (min > max): no offer to send
    }
    in.offered_versions = {versions.data(), version_count};
    in.key_share_group = share_group;
    in.key_share = hello.kx->public_value().bytes();
    in.cookie = cookie;
    if (cfg.verify_ip.empty()) {
        in.sni_host = cfg.sni_host;
    }
    in.alpn = cfg.alpn;
    if (psk_offered) {
        const TlsSuiteInfo *psk_info = tls_suite_info(session->suite);
        if (psk_info == nullptr || !psk_info->is_tls13) {
            return false;
        }
        in.has_psk = true;
        in.psk_identity = session->identity;
        in.psk_obfuscated_ticket_age = session->obfuscated_ticket_age;
        in.psk_binder_len = static_cast<std::uint8_t>(tls_hash_len(psk_info->hash));
        in.early_data = early_data_ext && !second; // HRR kills 0-RTT (06 §2.2)
    }

    const auto encoded = tls_encode_client_hello(in, hello.ch);
    if (!encoded.has_value()) {
        return false;
    }
    hello.len = encoded->len;
    hello.binder_off = in.has_psk ? encoded->binder_block_offset : 0;
    return true;
}

bool tls_client_backfill_psk_binder(TlsKeySchedule13 &sched, TlsClientHelloState &hello) noexcept {
    if (hello.binder_off == 0) {
        return true;
    }
    auto binder_key = sched.binder_key(TlsPskBinderKind::Resumption);
    if (!binder_key.has_value()) {
        return false;
    }
    const std::size_t mac_len = binder_key->len();
    TlsHash truncated;
    if (!truncated.init(sched.hash()) || !truncated.update({hello.ch.data(), hello.binder_off})) {
        return false;
    }
    std::array<std::uint8_t, 64> digest{};
    if (!truncated.final(digest)) {
        return false;
    }
    // Binder bytes start 3 past the binders-length prefix: be16(1+len),
    // u8(len), then the MAC.
    return tls13_psk_binder_mac(*binder_key, {digest.data(), mac_len},
                                {hello.ch.data() + hello.binder_off + 3, mac_len})
            .has_value();
}

bool tls_client_init_early_write(TlsKeySchedule13 &sched, const TlsSessionOffer &session,
                                 const TlsClientHelloState &hello, TlsRecordCipher &write) noexcept {
    TlsHash one_shot;
    if (!one_shot.init(sched.hash()) || !one_shot.update({hello.ch.data(), hello.len})) {
        return false;
    }
    std::array<std::uint8_t, 64> digest{};
    if (!one_shot.final(digest)) {
        return false;
    }
    auto early_secret = sched.client_early_traffic_secret({digest.data(), tls_hash_len(sched.hash())});
    if (!early_secret.has_value()) {
        return false;
    }
    TlsRecordCipher fresh;
    auto derived = tls13_traffic_keys(*early_secret, session.suite);
    if (!derived.has_value()) {
        return false;
    }
    TlsTrafficKeys &keys = *derived;
    if (!fresh.init(session.suite, TlsRecordProtectionKind::Tls13, {keys.key.data(), keys.key_len},
                    {keys.iv.data(), keys.iv_len})
                 .has_value()) {
        return false;
    }
    tls_secure_wipe(keys.key.data(), keys.key.size());
    tls_secure_wipe(keys.iv.data(), keys.iv.size());
    write = std::move(fresh);
    return true;
}

} // namespace fiber::tls
