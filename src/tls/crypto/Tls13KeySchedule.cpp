#include <fiber/tls/crypto/Tls13KeySchedule.h>

#include <array>
#include <cstring>
#include <string_view>

#include <fiber/common/Assert.h>

#include "TlsCryptoPrimitives.h"

namespace fiber::tls {

// ---------------------------------------------------------------------------
// 1.3 internals: HKDF-Expand-Label and the derivation tree
// ---------------------------------------------------------------------------

namespace {

constexpr std::string_view kTls13LabelPrefix = "tls13 "; // same constant as QUIC's

constexpr std::string_view kLabelDerived = "derived";
constexpr std::string_view kLabelExternalBinder = "ext binder";
constexpr std::string_view kLabelResumptionBinder = "res binder";
constexpr std::string_view kLabelClientEarly = "c e traffic";
constexpr std::string_view kLabelClientHs = "c hs traffic";
constexpr std::string_view kLabelServerHs = "s hs traffic";
constexpr std::string_view kLabelClientApp = "c ap traffic";
constexpr std::string_view kLabelServerApp = "s ap traffic";
constexpr std::string_view kLabelResumptionMaster = "res master";
constexpr std::string_view kLabelResumptionPsk = "resumption";
constexpr std::string_view kLabelTrafficUpdate = "traffic upd";
constexpr std::string_view kLabelFinished = "finished";
constexpr std::string_view kLabelKey = "key";
constexpr std::string_view kLabelIv = "iv";

// HkdfLabel = BE16(length) || u8(label_len) || "tls13 " || label ||
//             u8(context_len) || context  (RFC 8446 §7.1)
[[nodiscard]] bool expand_label(std::span<std::uint8_t> out, TlsHashAlgorithm hash,
                                std::span<const std::uint8_t> secret, std::string_view label,
                                std::span<const std::uint8_t> context) noexcept {
    FIBER_ASSERT(!out.empty() && out.size() <= 0xFFFF);
    FIBER_ASSERT(kTls13LabelPrefix.size() + label.size() <= 255);
    FIBER_ASSERT(context.size() <= 255);
    FIBER_ASSERT(2 + 1 + kTls13LabelPrefix.size() + label.size() + 1 + context.size() <= 255);

    std::uint8_t info[255];
    std::size_t n = 0;
    info[n++] = static_cast<std::uint8_t>(out.size() >> 8);
    info[n++] = static_cast<std::uint8_t>(out.size());
    info[n++] = static_cast<std::uint8_t>(kTls13LabelPrefix.size() + label.size());
    std::memcpy(info + n, kTls13LabelPrefix.data(), kTls13LabelPrefix.size());
    n += kTls13LabelPrefix.size();
    std::memcpy(info + n, label.data(), label.size());
    n += label.size();
    info[n++] = static_cast<std::uint8_t>(context.size());
    if (!context.empty()) { // an empty context may be a null span: memcpy(_, nullptr, 0) is UB
        std::memcpy(info + n, context.data(), context.size());
    }
    n += context.size();
    return tls_hkdf_expand(out, hash, secret, {info, n});
}

// Derive-Secret(secret, label, transcript_hash) — context = the hash bytes.
[[nodiscard]] common::IoResult<TlsSecret> derive_secret(const TlsSecret &secret, std::string_view label,
                                                        std::span<const std::uint8_t> transcript_hash,
                                                        TlsHashAlgorithm hash, std::uint8_t hash_len) noexcept {
    std::array<std::uint8_t, TlsSecret::kMaxLen> buf{};
    if (!expand_label({buf.data(), hash_len}, hash, secret.bytes(), label, transcript_hash)) {
        tls_secure_wipe(buf.data(), buf.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    TlsSecret out = TlsSecret::from_bytes({buf.data(), hash_len});
    tls_secure_wipe(buf.data(), buf.size());
    return out;
}

// Derive-Secret(secret, label, "") — the "" is the empty message SEQUENCE, so
// the context is Transcript-Hash("") = Hash(nothing): a full-length digest
// (SHA-256 e3 b0 c4 42 ...), never a zero-length span. Pinned by RFC 8448
// §3/§4 ("tls13 derived", "res binder"); contrast "finished"/"key"/"iv"/
// "traffic upd", whose empty context is a literal zero-length span.
[[nodiscard]] common::IoResult<TlsSecret> derive_secret_empty_transcript(const TlsSecret &secret,
                                                                         std::string_view label, TlsHashAlgorithm hash,
                                                                         std::uint8_t hash_len) noexcept {
    std::array<std::uint8_t, TlsSecret::kMaxLen> ctx{};
    if (!tls_digest_empty({ctx.data(), hash_len}, hash)) {
        return std::unexpected(common::IoErr::Unknown);
    }
    common::IoResult<TlsSecret> out = derive_secret(secret, label, {ctx.data(), hash_len}, hash, hash_len);
    tls_secure_wipe(ctx.data(), ctx.size());
    return out;
}

// HKDF-Extract(salt, ikm) into a TlsSecret-sized buffer. The scratch is
// EVP_MAX_MD_SIZE (64) per the adapter contract, even though only hash_len
// bytes are meaningful.
[[nodiscard]] common::IoResult<TlsSecret> extract_secret(std::span<const std::uint8_t> salt,
                                                         std::span<const std::uint8_t> ikm, TlsHashAlgorithm hash,
                                                         std::uint8_t hash_len) noexcept {
    std::array<std::uint8_t, 64> buf{};
    if (!tls_hkdf_extract(buf, hash, ikm, salt)) {
        tls_secure_wipe(buf.data(), buf.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    TlsSecret out = TlsSecret::from_bytes({buf.data(), hash_len});
    tls_secure_wipe(buf.data(), buf.size());
    return out;
}

[[nodiscard]] const TlsSuiteInfo &suite_info_or_assert(TlsCipherSuiteId suite, bool want_tls13) noexcept {
    const TlsSuiteInfo *info = tls_suite_info(suite);
    FIBER_ASSERT(info != nullptr && info->is_tls13 == want_tls13);
    return *info;
}

} // namespace

// ---------------------------------------------------------------------------
// TlsKeySchedule13
// ---------------------------------------------------------------------------

TlsKeySchedule13::TlsKeySchedule13(TlsCipherSuiteId suite) noexcept :
    suite_(suite), hash_(suite_info_or_assert(suite, true).hash),
    hash_len_(static_cast<std::uint8_t>(tls_hash_len(hash_))) {
    // Early Secret with no PSK: HKDF-Extract(zeros(hash_len), zeros(hash_len)).
    // The no-PSK "" in the RFC 8446 diagram means zeros(hash_len) as IKM —
    // NOT the empty string (pinned by RFC 8448 §3: IKM = 32 zero octets,
    // secret = 33 ad 0a 1c ...). Zero-length and zeros(hash_len) salt are
    // equivalent under HMAC; the explicit zeros mirror BoringSSL's
    // tls13_init_key_schedule. A failed extract here is an unusable crypto
    // library — unrecoverable.
    std::array<std::uint8_t, TlsSecret::kMaxLen> zero_salt{};
    std::array<std::uint8_t, 64> buf{};
    FIBER_ASSERT(tls_hkdf_extract(buf, hash_, {zero_salt.data(), hash_len_}, {zero_salt.data(), hash_len_}));
    early_secret_ = TlsSecret::from_bytes({buf.data(), hash_len_});
    tls_secure_wipe(buf.data(), buf.size());
}

TlsKeySchedule13::~TlsKeySchedule13() {
    early_secret_.wipe();
    handshake_secret_.wipe();
    master_secret_.wipe();
}

common::IoResult<void> TlsKeySchedule13::set_psk(std::span<const std::uint8_t> psk) noexcept {
    FIBER_ASSERT(stage_ == Stage::Early && !binder_done_ && !early_done_ && !psk_set_);
    psk_set_ = true;

    std::array<std::uint8_t, TlsSecret::kMaxLen> zero_salt{};
    // An empty PSK normalizes to zeros(hash_len): same Early Secret as the
    // no-PSK construction, so set_psk({}) == the constructor default.
    const std::span<const std::uint8_t> ikm =
            psk.empty() ? std::span<const std::uint8_t>{zero_salt.data(), hash_len_} : psk;
    std::array<std::uint8_t, 64> buf{};
    if (!tls_hkdf_extract(buf, hash_, ikm, {zero_salt.data(), hash_len_})) {
        tls_secure_wipe(buf.data(), buf.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    early_secret_ = TlsSecret::from_bytes({buf.data(), hash_len_});
    tls_secure_wipe(buf.data(), buf.size());
    return {};
}

common::IoResult<TlsSecret> TlsKeySchedule13::binder_key(TlsPskBinderKind kind) noexcept {
    FIBER_ASSERT(stage_ == Stage::Early && !binder_done_ && psk_set_);
    binder_done_ = true;
    return derive_secret_empty_transcript(
            early_secret_, kind == TlsPskBinderKind::External ? kLabelExternalBinder : kLabelResumptionBinder, hash_,
            hash_len_);
}

common::IoResult<TlsSecret>
TlsKeySchedule13::client_early_traffic_secret(std::span<const std::uint8_t> hash_client_hello) noexcept {
    FIBER_ASSERT(stage_ == Stage::Early && !early_done_);
    FIBER_ASSERT(hash_client_hello.size() == hash_len_);
    early_done_ = true;
    return derive_secret(early_secret_, kLabelClientEarly, hash_client_hello, hash_, hash_len_);
}

common::IoResult<void> TlsKeySchedule13::handshake_secrets(std::span<const std::uint8_t> z,
                                                           std::span<const std::uint8_t> hash_ch_sh,
                                                           TlsSecret &client_hs, TlsSecret &server_hs) noexcept {
    FIBER_ASSERT(stage_ == Stage::Early && z.size() == 32 && hash_ch_sh.size() == hash_len_);

    common::IoResult<TlsSecret> derived =
            derive_secret_empty_transcript(early_secret_, kLabelDerived, hash_, hash_len_);
    if (!derived.has_value()) {
        return std::unexpected(derived.error());
    }
    common::IoResult<TlsSecret> handshake = extract_secret(derived->bytes(), z, hash_, hash_len_);
    derived->wipe();
    if (!handshake.has_value()) {
        return std::unexpected(handshake.error());
    }
    handshake_secret_ = std::move(*handshake);

    common::IoResult<TlsSecret> client = derive_secret(handshake_secret_, kLabelClientHs, hash_ch_sh, hash_, hash_len_);
    if (!client.has_value()) {
        return std::unexpected(client.error());
    }
    common::IoResult<TlsSecret> server = derive_secret(handshake_secret_, kLabelServerHs, hash_ch_sh, hash_, hash_len_);
    if (!server.has_value()) {
        return std::unexpected(server.error());
    }
    client_hs = std::move(*client);
    server_hs = std::move(*server);
    stage_ = Stage::Handshake;
    return {};
}

common::IoResult<void> TlsKeySchedule13::application_secrets(std::span<const std::uint8_t> hash_ch_server_fin,
                                                             TlsSecret &client_app0, TlsSecret &server_app0) noexcept {
    FIBER_ASSERT(stage_ == Stage::Handshake && hash_ch_server_fin.size() == hash_len_);

    common::IoResult<TlsSecret> derived =
            derive_secret_empty_transcript(handshake_secret_, kLabelDerived, hash_, hash_len_);
    if (!derived.has_value()) {
        return std::unexpected(derived.error());
    }
    // Master Secret: HKDF-Extract(derived, zeros(hash_len)) — like the Early
    // Secret's no-PSK input, the "" IKM is zeros(hash_len), not empty (RFC
    // 8448 §3 line ~327: IKM = 32 zero octets).
    std::array<std::uint8_t, TlsSecret::kMaxLen> zero_ikm{};
    common::IoResult<TlsSecret> master =
            extract_secret(derived->bytes(), {zero_ikm.data(), hash_len_}, hash_, hash_len_);
    tls_secure_wipe(zero_ikm.data(), zero_ikm.size());
    derived->wipe();
    if (!master.has_value()) {
        return std::unexpected(master.error());
    }
    master_secret_ = std::move(*master);

    common::IoResult<TlsSecret> client =
            derive_secret(master_secret_, kLabelClientApp, hash_ch_server_fin, hash_, hash_len_);
    if (!client.has_value()) {
        return std::unexpected(client.error());
    }
    common::IoResult<TlsSecret> server =
            derive_secret(master_secret_, kLabelServerApp, hash_ch_server_fin, hash_, hash_len_);
    if (!server.has_value()) {
        return std::unexpected(server.error());
    }
    client_app0 = std::move(*client);
    server_app0 = std::move(*server);
    stage_ = Stage::Application;
    return {};
}

common::IoResult<TlsSecret>
TlsKeySchedule13::resumption_master_secret(std::span<const std::uint8_t> hash_ch_client_fin) noexcept {
    FIBER_ASSERT(stage_ == Stage::Application && !resumption_done_ && hash_ch_client_fin.size() == hash_len_);
    resumption_done_ = true;
    return derive_secret(master_secret_, kLabelResumptionMaster, hash_ch_client_fin, hash_, hash_len_);
}

// ---------------------------------------------------------------------------
// free functions
// ---------------------------------------------------------------------------

common::IoResult<TlsTrafficKeys> tls13_traffic_keys(const TlsSecret &secret, TlsCipherSuiteId suite) noexcept {
    const TlsSuiteInfo &info = suite_info_or_assert(suite, true);
    FIBER_ASSERT(secret.len() == tls_hash_len(info.hash));

    TlsTrafficKeys keys;
    keys.key_len = info.key_len;
    keys.iv_len = 12;
    if (!expand_label({keys.key.data(), info.key_len}, info.hash, secret.bytes(), kLabelKey, {}) ||
        !expand_label(keys.iv, info.hash, secret.bytes(), kLabelIv, {})) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return keys;
}

common::IoResult<TlsSecret> tls13_key_update(const TlsSecret &current) noexcept {
    FIBER_ASSERT(current.len() == 32 || current.len() == 48);
    const TlsHashAlgorithm hash = current.len() == 32 ? TlsHashAlgorithm::Sha256 : TlsHashAlgorithm::Sha384;
    return derive_secret(current, kLabelTrafficUpdate, {}, hash, current.len());
}

common::IoResult<TlsSecret> tls13_resumption_psk(const TlsSecret &resumption_master,
                                                 std::span<const std::uint8_t> nonce) noexcept {
    FIBER_ASSERT(resumption_master.len() == 32 || resumption_master.len() == 48);
    FIBER_ASSERT(nonce.size() <= 255);
    const TlsHashAlgorithm hash = resumption_master.len() == 32 ? TlsHashAlgorithm::Sha256 : TlsHashAlgorithm::Sha384;
    // Expand-Label(secret, label, context): here the nonce IS the context —
    // unlike "finished"/"key"/"iv" whose empty context is a literal empty span.
    return derive_secret(resumption_master, kLabelResumptionPsk, nonce, hash, resumption_master.len());
}

common::IoResult<void> tls13_finished_mac(const TlsSecret &traffic_secret,
                                          std::span<const std::uint8_t> transcript_hash,
                                          std::span<std::uint8_t> out_mac) noexcept {
    FIBER_ASSERT(out_mac.size() >= traffic_secret.len());
    FIBER_ASSERT(transcript_hash.size() == traffic_secret.len());

    const TlsHashAlgorithm hash = traffic_secret.len() == 32 ? TlsHashAlgorithm::Sha256 : TlsHashAlgorithm::Sha384;
    std::array<std::uint8_t, TlsSecret::kMaxLen> buf{};
    if (!expand_label({buf.data(), traffic_secret.len()}, hash, traffic_secret.bytes(), kLabelFinished, {})) {
        tls_secure_wipe(buf.data(), buf.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    TlsSecret finished_key = TlsSecret::from_bytes({buf.data(), traffic_secret.len()});
    tls_secure_wipe(buf.data(), buf.size());

    TlsHmac hmac;
    bool ok = hmac.init(hash, finished_key.bytes()) && hmac.update(transcript_hash) &&
              hmac.final({out_mac.data(), traffic_secret.len()});
    finished_key.wipe();
    if (!ok) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return {};
}

common::IoResult<void> tls13_psk_binder_mac(const TlsSecret &binder_key,
                                            std::span<const std::uint8_t> truncated_ch_hash,
                                            std::span<std::uint8_t> out_mac) noexcept {
    FIBER_ASSERT(out_mac.size() >= binder_key.len());

    // The binder uses the FINISHED construction (RFC 8446 §4.2.11.2, "is
    // computed in the same way as the Finished message"): the binder_key is
    // first expanded into a finished key, then MACed — not used as an HMAC
    // key directly. Pinned by RFC 8448 §4 (binder = 3a dd 4f b2 ...).
    const TlsHashAlgorithm hash = binder_key.len() == 32 ? TlsHashAlgorithm::Sha256 : TlsHashAlgorithm::Sha384;
    std::array<std::uint8_t, TlsSecret::kMaxLen> buf{};
    if (!expand_label({buf.data(), binder_key.len()}, hash, binder_key.bytes(), kLabelFinished, {})) {
        tls_secure_wipe(buf.data(), buf.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    TlsSecret finished_key = TlsSecret::from_bytes({buf.data(), binder_key.len()});
    tls_secure_wipe(buf.data(), buf.size());

    TlsHmac hmac;
    bool ok = hmac.init(hash, finished_key.bytes()) && hmac.update(truncated_ch_hash) &&
              hmac.final({out_mac.data(), binder_key.len()});
    finished_key.wipe();
    if (!ok) {
        return std::unexpected(common::IoErr::Unknown);
    }
    return {};
}

} // namespace fiber::tls
