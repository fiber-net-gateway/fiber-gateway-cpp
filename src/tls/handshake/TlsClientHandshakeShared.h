#ifndef FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_SHARED_H
#define FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_SHARED_H

// Version-neutral pieces shared by the client handshake engine's outer shell
// and its two version sub-flows (06 §4.1): the offer tables every ServerHello
// validation checks against, the retained-ClientHello state the fork hands
// over, the 0-RTT write window, the terminal outcome channel, and the
// ClientHello construction helpers (CH1 in the outer shell, CH2 in the 1.3
// sub-flow's HRR path). Internal to src/tls.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "TlsSuitePreference.h"

#include <fiber/tls/TlsConfig.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include <fiber/tls/crypto/TlsKeyExchange.h>
#include <fiber/tls/crypto/TlsSignature.h>
#include <fiber/tls/record/TlsRecordCipher.h>

namespace fiber::tls {

// ---- engine-fixed offer tables (06 §5.4: registry constants, not config) ----

// Suites: kTlsSuitePreference in its hardware-aware effective form
// (TlsSuitePreference.h; 0xC030 = ECDHE-RSA-AES256-GCM — the IANA value,
// 0x0030 was never a suite) — the CH offers tls_effective_suite_order()
// verbatim; the membership check below scans kTlsSuitePreference.

// Share order: X25519 leads (the CH1 share, the only key_share sent); P-256
// and P-384 are advertised in supported_groups only, reached by an HRR (1.3)
// or the server's SKE curve (1.2). P-384 also matters in 1.2 beyond ECDHE:
// supported_groups bounds the server certificate's ECDSA curve (RFC 8422
// §5.1), so without it a P-384-certificate server is unreachable.
inline constexpr std::array<std::uint16_t, 3> kOfferedGroups{0x001D, 0x0017, 0x0018};

// The CH signature_algorithms offer = the 02b 1.2 preference (a superset:
// rsa_pkcs1_* only negotiates in 1.2; the 1.3 verify path gates by version).
inline constexpr auto kOfferedSigalgs = [] {
    std::array<std::uint16_t, kTls12SignaturePreference.size()> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint16_t>(kTls12SignaturePreference[i]);
    }
    return out;
}();

// Shared client-flight bounds (both sub-flows stage their flight in the
// outer-owned scratch). The CR sigalgs store holds only schemes we can sign
// with (tls_client_keep_cr_sigalgs), so our larger preference table bounds it.
inline constexpr std::size_t kClientMaxCrSigalgs = kTls12SignaturePreference.size() > kTls13SignaturePreference.size()
                                                           ? kTls12SignaturePreference.size()
                                                           : kTls13SignaturePreference.size();
inline constexpr std::size_t kClientMaxSigLen = 1024; // RSA-4096 signature bound

// A CertificateRequest's signature_algorithms may list far more schemes than
// we can sign with (OpenSSL sends 20; the vector allows 32767), so the list
// length is never an error. CertificateVerify selection only tests whether
// each of OUR schemes is in the list, so keep exactly those, deduplicated
// and in `ours` order. `raw` is the even-length u16 list; returns the count.
template<std::size_t N>
[[nodiscard]] std::size_t tls_client_keep_cr_sigalgs(std::span<const std::uint8_t> raw,
                                                     const std::array<TlsSignatureScheme, N> &ours,
                                                     std::array<std::uint16_t, kClientMaxCrSigalgs> &out) noexcept {
    static_assert(N <= kClientMaxCrSigalgs);
    std::size_t n = 0;
    for (const TlsSignatureScheme scheme: ours) {
        const auto code = static_cast<std::uint16_t>(scheme);
        for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            if (static_cast<std::uint16_t>((raw[i] << 8) | raw[i + 1]) == code) {
                out[n++] = code;
                break;
            }
        }
    }
    return n;
}

// Did the CH offer this suite? (The effective order is a permutation of
// kTlsSuitePreference, so membership is order-blind.)
[[nodiscard]] constexpr bool tls_client_suite_offered(std::uint16_t raw) noexcept {
    for (const std::uint16_t offered: kTlsSuitePreference) {
        if (offered == raw) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool tls_client_group_offered(std::uint16_t raw) noexcept {
    for (const std::uint16_t offered: kOfferedGroups) {
        if (raw == offered) {
            return true;
        }
    }
    return false;
}

// The SKE/CV scheme must be one the CH offered (kOfferedSigalgs is a superset
// of every 1.2- and 1.3-negotiable scheme).
[[nodiscard]] constexpr bool tls_client_sigalg_offered(std::uint16_t raw) noexcept {
    for (const std::uint16_t scheme: kOfferedSigalgs) {
        if (scheme == raw) {
            return true;
        }
    }
    return false;
}

// ---- pre-fork state owned by the outer shell, borrowed by the sub-flows ----

// The retained ClientHello and its keying material. Outer-owned from
// construction (the first flight is version-neutral); the forked sub-flow
// borrows it MUTABLY — the 1.3 HRR path rewrites CH2 in place, the 1.2 SKE
// path rebuilds the exchange onto the server's curve.
struct TlsClientHelloState {
    static constexpr std::size_t kCap = 8192; // retained ClientHello bytes

    std::array<std::uint8_t, kCap> ch{};
    std::size_t len = 0;
    std::size_t binder_off = 0; // message-relative binder-block offset (0 = no PSK)
    std::unique_ptr<TlsKeyExchange> kx; // the share in `ch` (rebuilt at HRR / 1.2 SKE)
    TlsNamedGroup kx_group = TlsNamedGroup::X25519; // group of the share in `ch`
    std::array<std::uint8_t, 32> client_random{};
    std::array<std::uint8_t, 32> session_id{};
};

// The 0-RTT write window. Opened before the version is known (the early
// flight follows CH1), so the outer shell owns it — write_early_data() needs
// no version dispatch. Only the 1.3 sub-flow closes it (SH read point, HRR,
// EOED); a 1.2 fork closes it at the read point in the outer shell.
struct TlsClientEarlyWindow {
    TlsRecordCipher write; // early-traffic cipher (initialized when offered)
    std::size_t written = 0; // cumulative early bytes vs session->max_early_data
    bool offered_ext = false; // early_data extension is in the CH
    bool closed = false; // write_early_data() is no longer valid
};

// Terminal handshake outcome — written by whichever sub-flow ran (or by the
// outer shell before the fork), read by the public API.
struct TlsClientHandshakeOutcome {
    bool done = false;
    bool failed = false;
    TlsAlertDesc alert = TlsAlertDesc::InternalError;
};

// ---- ClientHello construction (CH1 in the outer shell, CH2 under HRR) ----

// create + generate composed: the CH1 first flight, the HRR rebuild, and the
// 1.2 SKE group switch all arrive here. Failure is an internal error
// (allocation) — the engine maps it to internal_error.
[[nodiscard]] common::IoResult<std::unique_ptr<TlsKeyExchange>> tls_client_kx_offer(TlsNamedGroup group) noexcept;

// Rebuilds `hello.kx` for `share_group` (fresh keypair every call — the CH1
// generator and the HRR rebuild both arrive here), encodes the hello into
// `hello.ch`, and draws fresh random/session_id bytes only when !second
// (RFC 8446 §4.1.4: CH2 keeps CH1's). `psk_offered` requires `session` and
// pre-fills the binder placeholder the caller backfills.
[[nodiscard]] bool tls_client_hello_build(TlsClientHelloState &hello, const TlsClientConfig &cfg,
                                          const TlsSessionOffer *session, bool psk_offered, bool early_data_ext,
                                          bool second, std::uint16_t share_group,
                                          std::span<const std::uint8_t> cookie) noexcept;

// Computes and backfills the PSK binder over the truncated retained CH
// (writes into hello.ch). Requires `sched` freshly set_psk'd: binder_key is
// once-per-schedule, so HRR rebuilds the schedule before calling this again.
[[nodiscard]] bool tls_client_backfill_psk_binder(TlsKeySchedule13 &sched, TlsClientHelloState &hello) noexcept;

// Derives client_early_traffic_secret from Hash(CH) — the derivation half
// of the 0-RTT write path, split for QUIC (10 定谳 5: early data is STREAM
// frames there, so only the secret is wanted, never a record cipher).
[[nodiscard]] common::IoResult<TlsSecret> tls_client_early_secret(TlsKeySchedule13 &sched,
                                                                  const TlsClientHelloState &hello) noexcept;

// Derives the early-traffic cipher from Hash(CH) — the pre-fork 0-RTT write
// instance (both CH1-attached early data and the window the fork inherits).
[[nodiscard]] bool tls_client_init_early_write(TlsKeySchedule13 &sched, const TlsSessionOffer &session,
                                               const TlsClientHelloState &hello, TlsRecordCipher &write) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_CLIENT_HANDSHAKE_SHARED_H
