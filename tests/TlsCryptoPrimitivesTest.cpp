#include <gtest/gtest.h>

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "tls/crypto/TlsCryptoPrimitives.h" // src-side adapter header (tests may include it)

using namespace fiber::tls;

namespace {

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

// One-shot reference straight from the EVP, sharing no code path with TlsHash.
std::vector<std::uint8_t> ref_digest(TlsHashAlgorithm hash, std::span<const std::uint8_t> data) {
    const EVP_MD *md = hash == TlsHashAlgorithm::Sha256 ? EVP_sha256() : EVP_sha384();
    std::vector<std::uint8_t> out(hash == TlsHashAlgorithm::Sha256 ? 32 : 48);
    unsigned len = 0;
    EXPECT_EQ(1, EVP_Digest(data.data(), data.size(), out.data(), &len, md, nullptr));
    return out;
}

void expect_digest(std::span<const std::uint8_t> got, std::span<const std::uint8_t> want) {
    ASSERT_EQ(want.size(), got.size());
    EXPECT_EQ(0, std::memcmp(got.data(), want.data(), got.size()));
}

// Incremental feeding in awkward chunk sizes must equal the one-shot digest —
// the property the running transcript hash depends on.
TEST(TlsHash, IncrementalEqualsOneShot) {
    const TlsHashAlgorithm algos[] = {TlsHashAlgorithm::Sha256, TlsHashAlgorithm::Sha384};
    for (const TlsHashAlgorithm hash: algos) {
        const auto data = ramp(1000, 0x10);
        for (const std::size_t chunk: {std::size_t{1}, std::size_t{3}, std::size_t{63}, std::size_t{64},
                                       std::size_t{65}, std::size_t{128}, std::size_t{1000}}) {
            TlsHash h;
            ASSERT_TRUE(h.init(hash));
            for (std::size_t off = 0; off < data.size(); off += chunk) {
                const std::size_t take = std::min(chunk, data.size() - off);
                ASSERT_TRUE(h.update({data.data() + off, take}));
            }
            std::array<std::uint8_t, 64> out{};
            ASSERT_TRUE(h.final(out));
            expect_digest({out.data(), tls_hash_len(hash)}, ref_digest(hash, data));
        }

        // Empty input matches Hash("") — the TLS 1.3 empty-transcript context.
        TlsHash h;
        ASSERT_TRUE(h.init(hash));
        std::array<std::uint8_t, 64> out{};
        ASSERT_TRUE(h.final(out));
        std::array<std::uint8_t, 64> empty{};
        ASSERT_TRUE(tls_digest_empty(empty, hash));
        expect_digest({out.data(), tls_hash_len(hash)}, {empty.data(), tls_hash_len(hash)});
    }
}

// Empty updates are no-ops (the transcript may be handed empty spans).
TEST(TlsHash, EmptyUpdateIsNoOp) {
    TlsHash h;
    ASSERT_TRUE(h.init(TlsHashAlgorithm::Sha256));
    ASSERT_TRUE(h.update({}));
    const auto data = ramp(50, 0x20);
    ASSERT_TRUE(h.update(data));
    ASSERT_TRUE(h.update({}));
    std::array<std::uint8_t, 64> out{};
    ASSERT_TRUE(h.final(out));
    expect_digest({out.data(), 32}, ref_digest(TlsHashAlgorithm::Sha256, data));
}

// Copy = fork: the transcript forks by copying the hash state, so a mid-stream
// copy continued with a different suffix must match the independent digest of
// prefix+suffix — for BOTH suffixes from the same fork point.
TEST(TlsHash, CopyForksTheStream) {
    const auto prefix = ramp(300, 0x30);
    const auto suffix_a = ramp(200, 0x40);
    const auto suffix_b = ramp(400, 0x50);

    TlsHash base;
    ASSERT_TRUE(base.init(TlsHashAlgorithm::Sha256));
    ASSERT_TRUE(base.update(prefix));

    TlsHash fork = base; // trivially copyable state
    std::array<std::uint8_t, 32> a{}, b{};
    ASSERT_TRUE(base.update(suffix_a) && base.final(a));
    ASSERT_TRUE(fork.update(suffix_b) && fork.final(b));

    std::vector<std::uint8_t> wa(prefix), wb(prefix);
    wa.insert(wa.end(), suffix_a.begin(), suffix_a.end());
    wb.insert(wb.end(), suffix_b.begin(), suffix_b.end());
    expect_digest({a.data(), 32}, ref_digest(TlsHashAlgorithm::Sha256, wa));
    expect_digest({b.data(), 32}, ref_digest(TlsHashAlgorithm::Sha256, wb));
}

} // namespace
