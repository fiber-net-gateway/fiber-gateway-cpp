#include <fiber/tls/crypto/Tls12KeySchedule.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <fiber/common/Assert.h>

#include "TlsCryptoPrimitives.h"

namespace fiber::tls {

// ---------------------------------------------------------------------------
// TLS 1.2 PRF (RFC 5246 §5)
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] const TlsSuiteInfo &suite_info_or_assert(TlsCipherSuiteId suite, bool want_tls13) noexcept {
    const TlsSuiteInfo *info = tls_suite_info(suite);
    FIBER_ASSERT(info != nullptr && info->is_tls13 == want_tls13);
    return *info;
}

// P_hash: A(0) = seed', A(i) = HMAC(secret, A(i-1)),
// T(i) = HMAC(secret, A(i) || seed'), output = T(1) || T(2) || ...
// seed' = label || seed, assembled on the stack (label <= 22, seed <= 64).
constexpr std::string_view kPrfLabelMaster = "master secret";
constexpr std::string_view kPrfLabelExtendedMaster = "extended master secret";
constexpr std::string_view kPrfLabelKeyExpansion = "key expansion";
constexpr std::string_view kPrfLabelClientFinished = "client finished";
constexpr std::string_view kPrfLabelServerFinished = "server finished";

constexpr std::size_t kPrfLabelSeedCap = 96; // longest use: 22 + 48 (EMS session_hash)

[[nodiscard]] bool prf(TlsHashAlgorithm hash, std::span<const std::uint8_t> secret, std::string_view label,
                       std::span<const std::uint8_t> seed, std::span<std::uint8_t> out) noexcept {
    const std::size_t hash_len = tls_hash_len(hash);
    FIBER_ASSERT(!out.empty() && out.size() <= 128); // largest consumer: key_block = 88
    FIBER_ASSERT(label.size() <= kPrfLabelExtendedMaster.size()); // longest label we use (22)
    FIBER_ASSERT(label.size() + seed.size() <= kPrfLabelSeedCap);

    std::array<std::uint8_t, kPrfLabelSeedCap> label_seed{};
    std::size_t label_seed_len = 0;
    std::memcpy(label_seed.data(), label.data(), label.size());
    std::memcpy(label_seed.data() + label.size(), seed.data(), seed.size());
    label_seed_len = label.size() + seed.size();

    std::array<std::uint8_t, TlsSecret::kMaxLen> a{};
    std::array<std::uint8_t, TlsSecret::kMaxLen> t{};
    bool ok = true;
    {
        TlsHmac hmac;
        ok = hmac.init(hash, secret) && hmac.update({label_seed.data(), label_seed_len}) &&
             hmac.final({a.data(), hash_len});
    }
    for (std::size_t done = 0; ok && done < out.size(); done += hash_len) {
        TlsHmac hmac;
        std::array<std::uint8_t, TlsSecret::kMaxLen + kPrfLabelSeedCap> block{};
        std::memcpy(block.data(), a.data(), hash_len);
        std::memcpy(block.data() + hash_len, label_seed.data(), label_seed_len);
        ok = hmac.init(hash, secret) && hmac.update({block.data(), hash_len + label_seed_len}) &&
             hmac.final({t.data(), hash_len});
        tls_secure_wipe(block.data(), block.size());
        const std::size_t take = std::min(hash_len, out.size() - done);
        std::memcpy(out.data() + done, t.data(), take);
        if (done + hash_len < out.size()) {
            // A(i+1) = HMAC(secret, A(i)) — a chain SEPARATE from the T
            // outputs (RFC 5246 §5). Conflating the two (A(i+1) = T(i))
            // still yields a deterministic PRF but diverges from every real
            // peer from the second block on — first caught by interop.
            TlsHmac chain;
            std::array<std::uint8_t, TlsSecret::kMaxLen> next_a{};
            ok = chain.init(hash, secret) && chain.update({a.data(), hash_len}) &&
                 chain.final({next_a.data(), hash_len});
            tls_secure_wipe(a.data(), hash_len);
            std::memcpy(a.data(), next_a.data(), hash_len);
            tls_secure_wipe(next_a.data(), next_a.size());
        }
    }
    tls_secure_wipe(a.data(), a.size());
    tls_secure_wipe(t.data(), t.size());
    tls_secure_wipe(label_seed.data(), label_seed.size());
    return ok;
}

} // namespace

common::IoResult<TlsSecret> tls12_master_secret(TlsCipherSuiteId suite, std::span<const std::uint8_t> z,
                                                std::span<const std::uint8_t> client_random,
                                                std::span<const std::uint8_t> server_random) noexcept {
    const TlsSuiteInfo &info = suite_info_or_assert(suite, false);
    FIBER_ASSERT((z.size() == 32 || z.size() == 48) && client_random.size() == 32 && server_random.size() == 32);

    std::array<std::uint8_t, 64> seed{};
    std::memcpy(seed.data(), client_random.data(), 32);
    std::memcpy(seed.data() + 32, server_random.data(), 32);

    std::array<std::uint8_t, 48> out{};
    if (!prf(info.hash, z, kPrfLabelMaster, seed, out)) {
        tls_secure_wipe(seed.data(), seed.size());
        tls_secure_wipe(out.data(), out.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    tls_secure_wipe(seed.data(), seed.size());
    TlsSecret secret = TlsSecret::from_bytes(out);
    tls_secure_wipe(out.data(), out.size());
    return secret;
}

common::IoResult<TlsSecret> tls12_extended_master_secret(TlsCipherSuiteId suite, std::span<const std::uint8_t> z,
                                                         std::span<const std::uint8_t> session_hash) noexcept {
    const TlsSuiteInfo &info = suite_info_or_assert(suite, false);
    FIBER_ASSERT(z.size() == 32 || z.size() == 48); // premaster: X25519/P-256 or P-384
    FIBER_ASSERT(session_hash.size() == tls_hash_len(info.hash));

    std::array<std::uint8_t, 48> out{};
    if (!prf(info.hash, z, kPrfLabelExtendedMaster, session_hash, out)) {
        tls_secure_wipe(out.data(), out.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    TlsSecret secret = TlsSecret::from_bytes(out);
    tls_secure_wipe(out.data(), out.size());
    return secret;
}

common::IoResult<Tls12WriteKeys> tls12_key_block(TlsCipherSuiteId suite, const TlsSecret &master,
                                                 std::span<const std::uint8_t> client_random,
                                                 std::span<const std::uint8_t> server_random) noexcept {
    const TlsSuiteInfo &info = suite_info_or_assert(suite, false);
    FIBER_ASSERT(master.len() == 48); // RFC 5246 §6.3: always 48, both PRFs
    FIBER_ASSERT(client_random.size() == 32 && server_random.size() == 32);

    // Seed order reversed vs the master secret: server_random || client_random.
    std::array<std::uint8_t, 64> seed{};
    std::memcpy(seed.data(), server_random.data(), 32);
    std::memcpy(seed.data() + 32, client_random.data(), 32);

    // MAC keys are empty for AEAD suites: key_block = c_key || s_key ||
    // c_iv || s_iv. The fixed IV length is suite-shaped: 4 for RFC 5288 GCM
    // (the other 8 nonce bytes ride the wire), 12 for RFC 7905 ChaCha20 (the
    // whole nonce is implicit).
    const std::size_t iv_len = info.aead == TlsAeadAlgorithm::Chacha20Poly1305 ? 12 : 4;
    const std::size_t block_len = 2 * info.key_len + 2 * iv_len;
    std::array<std::uint8_t, 88> block{}; // largest: 2*32 key + 2*12 iv (chacha)
    if (!prf(info.hash, master.bytes(), kPrfLabelKeyExpansion, seed, {block.data(), block_len})) {
        tls_secure_wipe(seed.data(), seed.size());
        tls_secure_wipe(block.data(), block.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    tls_secure_wipe(seed.data(), seed.size());

    Tls12WriteKeys keys;
    keys.client.key_len = info.key_len;
    keys.client.iv_len = static_cast<std::uint8_t>(iv_len);
    keys.server.key_len = info.key_len;
    keys.server.iv_len = static_cast<std::uint8_t>(iv_len);
    std::memcpy(keys.client.key.data(), block.data(), info.key_len);
    std::memcpy(keys.server.key.data(), block.data() + info.key_len, info.key_len);
    std::memcpy(keys.client.iv.data(), block.data() + 2 * info.key_len, iv_len);
    std::memcpy(keys.server.iv.data(), block.data() + 2 * info.key_len + iv_len, iv_len);
    tls_secure_wipe(block.data(), block.size());
    return keys;
}

common::IoResult<std::array<std::uint8_t, 12>>
tls12_verify_data(TlsCipherSuiteId suite, const TlsSecret &master, bool client,
                  std::span<const std::uint8_t> handshake_hash) noexcept {
    const TlsSuiteInfo &info = suite_info_or_assert(suite, false);
    FIBER_ASSERT(master.len() == 48); // RFC 5246 §6.3: always 48, both PRFs
    FIBER_ASSERT(handshake_hash.size() == tls_hash_len(info.hash));

    std::array<std::uint8_t, 12> out{};
    if (!prf(info.hash, master.bytes(), client ? kPrfLabelClientFinished : kPrfLabelServerFinished, handshake_hash,
             out)) {
        tls_secure_wipe(out.data(), out.size());
        return std::unexpected(common::IoErr::Unknown);
    }
    return out;
}

} // namespace fiber::tls
