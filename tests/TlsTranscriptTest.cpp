#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "tls/crypto/TlsCryptoPrimitives.h" // src-side adapter (tests may include it)
#include "tls/handshake/TlsTranscript.h"

#include <fiber/tls/handshake/TlsHandshakeMessage.h>
#include "TlsRfc8448Constants.h"

using namespace fiber::tls;

namespace {

std::vector<std::uint8_t> message(std::uint8_t type, std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(kTlsHandshakeHeaderSize + len);
    out[0] = type;
    out[1] = 0;
    out[2] = static_cast<std::uint8_t>(len >> 8);
    out[3] = static_cast<std::uint8_t>(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[kTlsHandshakeHeaderSize + i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

// Independent reference: a one-shot TlsHash over the concatenation.
std::vector<std::uint8_t> ref_digest(TlsHashAlgorithm hash,
                                     std::initializer_list<std::span<const std::uint8_t>> parts) {
    TlsHash ref;
    EXPECT_TRUE(ref.init(hash));
    for (const auto part: parts) {
        EXPECT_TRUE(ref.update(part));
    }
    std::vector<std::uint8_t> out(64, 0);
    EXPECT_TRUE(ref.final(out));
    out.resize(tls_hash_len(hash));
    return out;
}

void expect_digest(std::span<const std::uint8_t> got, std::span<const std::uint8_t> want) {
    ASSERT_EQ(want.size(), got.size());
    EXPECT_EQ(0, std::memcmp(got.data(), want.data(), got.size()));
}

const TlsHashAlgorithm kAlgos[] = {TlsHashAlgorithm::Sha256, TlsHashAlgorithm::Sha384};

// Incremental feeding of whole messages equals the one-shot digest, and a
// snapshot does not consume the running state.
TEST(TlsTranscript13, IncrementalEqualsOneShotAndSnapshotIsNonDestructive) {
    for (const TlsHashAlgorithm hash: kAlgos) {
        const auto ch = message(1, 64, 0x10);
        const auto sh = message(2, 32, 0x20);
        const auto ee = message(8, 128, 0x30);

        TlsTranscript13 transcript;
        ASSERT_TRUE(transcript.init(hash));
        ASSERT_TRUE(transcript.update(ch));
        ASSERT_TRUE(transcript.update(sh));

        std::array<std::uint8_t, 64> mid{};
        ASSERT_TRUE(transcript.snapshot_digest({mid.data(), tls_hash_len(hash)}));
        expect_digest({mid.data(), tls_hash_len(hash)}, ref_digest(hash, {ch, sh}));

        // snapshot is non-destructive: continue and compare against the
        // one-shot over all three messages; an earlier snapshot must equal
        // the prefix digest.
        ASSERT_TRUE(transcript.update(ee));
        std::array<std::uint8_t, 64> end{};
        ASSERT_TRUE(transcript.snapshot_digest({end.data(), tls_hash_len(hash)}));
        expect_digest({end.data(), tls_hash_len(hash)}, ref_digest(hash, {ch, sh, ee}));

        TlsHash prefix;
        ASSERT_TRUE(prefix.init(hash));
        ASSERT_TRUE(prefix.update(ch));
        ASSERT_TRUE(prefix.update(sh));
        std::array<std::uint8_t, 64> prefix_out{};
        ASSERT_TRUE(prefix.final(prefix_out));
        expect_digest({mid.data(), tls_hash_len(hash)}, {prefix_out.data(), tls_hash_len(hash)});
    }
}

// Fork = independent continuation: both branches match their one-shot
// references from the shared prefix.
TEST(TlsTranscript13, ForkDivergesIndependently) {
    const auto prefix_a = message(1, 96, 0x00);
    const auto prefix_b = message(2, 48, 0x40);
    const auto suffix_a = message(8, 32, 0x80);
    const auto suffix_b = message(11, 256, 0xC0);

    TlsTranscript13 base;
    ASSERT_TRUE(base.init(TlsHashAlgorithm::Sha256));
    ASSERT_TRUE(base.update(prefix_a));
    ASSERT_TRUE(base.update(prefix_b));

    TlsTranscript13 forked = base.fork();
    ASSERT_TRUE(base.update(suffix_a));
    ASSERT_TRUE(forked.update(suffix_b));

    std::array<std::uint8_t, 32> a{}, b{};
    ASSERT_TRUE(base.snapshot_digest(a));
    ASSERT_TRUE(forked.snapshot_digest(b));
    expect_digest(a, ref_digest(TlsHashAlgorithm::Sha256, {prefix_a, prefix_b, suffix_a}));
    expect_digest(b, ref_digest(TlsHashAlgorithm::Sha256, {prefix_a, prefix_b, suffix_b}));
}

// HRR restart: message_hash wraps ONLY Hash(CH1); the HRR and CH2 are hashed
// into the restarted transcript as plain messages — a hand-built hash over
// (254 || len || digest(CH1)) || HRR || CH2, the RFC 8446 §4.4.1 construction
// BoringSSL also implements, verified independently.
TEST(TlsTranscript13, RestartMessageHashMatchesHandBuilt) {
    for (const TlsHashAlgorithm hash: kAlgos) {
        const auto ch1 = message(1, 96, 0x00);
        const auto hrr = message(2, 32, 0x44);
        const auto ch2 = message(1, 64, 0x66);

        TlsTranscript13 transcript;
        ASSERT_TRUE(transcript.init(hash));
        ASSERT_TRUE(transcript.update(ch1));
        ASSERT_TRUE(transcript.restart_message_hash());
        ASSERT_TRUE(transcript.update(hrr));
        ASSERT_TRUE(transcript.update(ch2));

        // hand-built: message_hash header || digest(CH1) || HRR || CH2
        const std::size_t len = tls_hash_len(hash);
        std::vector<std::uint8_t> synthesized{254, 0x00, 0x00, static_cast<std::uint8_t>(len)};
        const auto inner = ref_digest(hash, {ch1});
        synthesized.insert(synthesized.end(), inner.begin(), inner.end());

        std::array<std::uint8_t, 64> got{};
        ASSERT_TRUE(transcript.snapshot_digest({got.data(), len}));
        expect_digest({got.data(), len}, ref_digest(hash, {synthesized, hrr, ch2}));
    }
}

// RFC 8448 §5 (HelloRetryRequest) vector: the restarted transcript equals
// the trace's "tls13 c hs traffic" context hash. Pins the §4.4.1 substitution
// against the RFC end to end — the constants are machine-generated
// (tests/TlsRfc8448Constants.h via temp/gen8448.py, re-verified with hashlib).
TEST(TlsTranscript13, Rfc8448Section5RestartMatchesTrace) {
    TlsTranscript13 transcript;
    ASSERT_TRUE(transcript.init(TlsHashAlgorithm::Sha256));
    ASSERT_TRUE(transcript.update(std::span<const std::uint8_t>{rfc8448::kS5Ch1}));
    ASSERT_TRUE(transcript.restart_message_hash());
    ASSERT_TRUE(transcript.update(std::span<const std::uint8_t>{rfc8448::kS5Hrr}));
    ASSERT_TRUE(transcript.update(std::span<const std::uint8_t>{rfc8448::kS5Ch2}));
    ASSERT_TRUE(transcript.update(std::span<const std::uint8_t>{rfc8448::kS5Sh2}));

    std::array<std::uint8_t, 32> got{};
    ASSERT_TRUE(transcript.snapshot_digest(got));
    expect_digest(got, std::span<const std::uint8_t>{rfc8448::kS5HashCh2Sh2});
}

TEST(TlsTranscript13, ContractViolationsFail) {
    TlsTranscript13 transcript; // uninitialized
    const auto ch = message(1, 16, 0x10);
    EXPECT_FALSE(transcript.update(ch));
    std::array<std::uint8_t, 32> out{};
    EXPECT_FALSE(transcript.snapshot_digest(out));
    EXPECT_FALSE(transcript.restart_message_hash());

    ASSERT_TRUE(transcript.init(TlsHashAlgorithm::Sha256));
    EXPECT_FALSE(transcript.snapshot_digest({out.data(), 31})); // wrong output size
    EXPECT_FALSE(transcript.snapshot_digest({out.data(), 48}));
    EXPECT_TRUE(transcript.snapshot_digest({out.data(), 32}));
}

TEST(TlsTranscript12, IncrementalEqualsOneShotAndContractViolationsFail) {
    for (const TlsHashAlgorithm hash: kAlgos) {
        const auto ch = message(1, 64, 0x10);
        const auto sh = message(2, 32, 0x20);
        const auto cert = message(11, 512, 0x30);

        TlsTranscript12 transcript;
        EXPECT_FALSE(transcript.update(ch)); // uninitialized
        std::array<std::uint8_t, 64> out{};
        EXPECT_FALSE(transcript.snapshot_digest({out.data(), tls_hash_len(hash)}));

        ASSERT_TRUE(transcript.init(hash));
        ASSERT_TRUE(transcript.update(ch));
        ASSERT_TRUE(transcript.update(sh));
        ASSERT_TRUE(transcript.update(cert));

        std::array<std::uint8_t, 64> digest{};
        ASSERT_TRUE(transcript.snapshot_digest({digest.data(), tls_hash_len(hash)}));
        expect_digest({digest.data(), tls_hash_len(hash)}, ref_digest(hash, {ch, sh, cert}));

        // non-destructive: snapshot twice, same value
        std::array<std::uint8_t, 64> again{};
        ASSERT_TRUE(transcript.snapshot_digest({again.data(), tls_hash_len(hash)}));
        EXPECT_EQ(0, std::memcmp(digest.data(), again.data(), tls_hash_len(hash)));

        EXPECT_FALSE(transcript.snapshot_digest(
                {out.data(), tls_hash_len(hash) == 32 ? std::size_t{48} : std::size_t{32}})); // wrong size
    }
}

} // namespace
