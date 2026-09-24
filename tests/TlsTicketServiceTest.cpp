#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <fiber/tls/TlsTicketService.h>
#include <fiber/tls/crypto/Tls13KeySchedule.h>

using namespace fiber::tls;

namespace {

constexpr std::int64_t kNow = 1'700'000'000'000;

constexpr std::size_t kHeaderLen = 17; // ver || key_id || nonce

bool secrets_equal(const TlsSecret &a, const TlsSecret &b) noexcept {
    return a.len() == b.len() && 0 == std::memcmp(a.bytes().data(), b.bytes().data(), a.len());
}

// The container's key_id field (bytes 1..4, big-endian) — which injected key
// minted this ticket.
[[nodiscard]] std::uint32_t key_id_of(const std::vector<std::uint8_t> &ticket) noexcept {
    return (static_cast<std::uint32_t>(ticket[1]) << 24) | (static_cast<std::uint32_t>(ticket[2]) << 16) |
           (static_cast<std::uint32_t>(ticket[3]) << 8) | static_cast<std::uint32_t>(ticket[4]);
}

struct Fixture {
    std::array<std::uint8_t, 32> master13{}; // SHA-256 schedule size
    std::array<std::uint8_t, 48> master12{};
    std::array<std::uint8_t, 16> key_bytes{}; // the injected TPK
    std::array<std::uint8_t, 16> key_bytes2{}; // the rotation successor TPK
    std::array<std::uint8_t, TlsTicketService::kMaxTicketLen> scratch{};

    Fixture() {
        for (std::size_t i = 0; i < master13.size(); ++i) {
            master13[i] = static_cast<std::uint8_t>(i * 7 + 1);
        }
        for (std::size_t i = 0; i < master12.size(); ++i) {
            master12[i] = static_cast<std::uint8_t>(i * 5 + 3);
        }
        for (std::size_t i = 0; i < key_bytes.size(); ++i) {
            key_bytes[i] = static_cast<std::uint8_t>(i * 11 + 5);
            key_bytes2[i] = static_cast<std::uint8_t>(i * 13 + 9);
        }
    }

    // The standard single-key set, born at `created`.
    [[nodiscard]] std::array<TlsTicketKeyMaterial, 1> keys(std::int64_t created = kNow) const noexcept {
        return {{{.id = 7, .bytes = key_bytes, .created_ms = created}}};
    }

    [[nodiscard]] TlsTicketRequest request13(std::string_view name = "example.com", std::uint32_t timeout_s = 3600,
                                             std::int64_t now = kNow) const noexcept {
        TlsTicketRequest req{};
        req.resumption_master = master13;
        req.ticket_nonce = 0;
        req.suite = TlsCipherSuiteId::TlsAes128GcmSha256;
        req.alpn = "h2";
        req.ticket_age_add = 0xdeadbeef;
        req.max_early_data = 0;
        req.timeout_s = timeout_s;
        req.now_unix_ms = now;
        req.version = TlsProtocolVersion::Tls13;
        req.name = name;
        return req;
    }

    [[nodiscard]] TlsTicketRequest request12(std::string_view name = "example.com", std::uint32_t timeout_s = 3600,
                                             std::int64_t now = kNow) const noexcept {
        TlsTicketRequest req = request13(name, timeout_s, now);
        req.resumption_master = master12;
        req.version = TlsProtocolVersion::Tls12;
        return req;
    }

    [[nodiscard]] std::vector<std::uint8_t> mint(TlsTicketService &service, const TlsTicketRequest &req) {
        const std::size_t len = TlsTicketService::mint_thunk(&service, req, scratch);
        EXPECT_GT(len, 0u);
        return {scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(len)};
    }
};

} // namespace

// ---- construction ----

TEST(TlsTicketService, EmptyKeySetIsInvalid) {
    Fixture fx;
    TlsTicketService service(std::span<const TlsTicketKeyMaterial>{}, TlsTicketKeyPolicy{});
    EXPECT_FALSE(service.valid());
    EXPECT_EQ(0u, service.key_count());
    // The minter hook stays safe: every mint declines.
    EXPECT_EQ(0u, TlsTicketService::mint_thunk(&service, fx.request13(), fx.scratch));
}

TEST(TlsTicketService, DuplicateIdsAreInvalid) {
    Fixture fx;
    const std::array<TlsTicketKeyMaterial, 2> keys{{
            {.id = 7, .bytes = fx.key_bytes, .created_ms = kNow},
            {.id = 7, .bytes = fx.key_bytes2, .created_ms = kNow + 1000},
    }};
    TlsTicketService service(keys, TlsTicketKeyPolicy{});
    EXPECT_FALSE(service.valid()); // open() would be ambiguous otherwise
}

// More keys than the ring holds: the newest survive, the oldest fall off.
TEST(TlsTicketService, OverflowKeepsNewestEight) {
    Fixture fx;
    std::array<TlsTicketKeyMaterial, 10> many{};
    for (std::size_t i = 0; i < many.size(); ++i) {
        many[i] = {.id = static_cast<std::uint32_t>(i + 1),
                   .bytes = fx.key_bytes,
                   .created_ms = static_cast<std::int64_t>(i) * 1000};
    }
    TlsTicketService service(many, TlsTicketKeyPolicy{});
    ASSERT_TRUE(service.valid());
    EXPECT_EQ(TlsTicketService::kMaxKeys, service.key_count());
    EXPECT_EQ(10u, key_id_of(fx.mint(service, fx.request13("example.com", 3600, 9'000))));
}

TEST(TlsTicketService, RandomKeyMintsFreshMaterial) {
    Fixture fx;
    TlsTicketKeyMaterial a{};
    TlsTicketKeyMaterial b{};
    ASSERT_TRUE(TlsTicketService::random_key(9, kNow, a));
    ASSERT_TRUE(TlsTicketService::random_key(9, kNow, b));
    EXPECT_EQ(9u, a.id);
    EXPECT_EQ(kNow, a.created_ms);
    EXPECT_NE(a.bytes, b.bytes); // fresh entropy per call

    const std::array<TlsTicketKeyMaterial, 1> keys{{a}};
    TlsTicketService service(keys, TlsTicketKeyPolicy{});
    ASSERT_TRUE(service.valid());
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13());
    TlsTicketContents contents;
    EXPECT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", kNow, contents));
}

// ---- mint/open round trips ----

TEST(TlsTicketService, Mint13RoundtripCarriesDerivedPsk) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    ASSERT_TRUE(service.valid());
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13());
    EXPECT_GT(ticket.size(), kHeaderLen + 16);
    EXPECT_EQ(7u, key_id_of(ticket)); // the injected key's id rides the container

    TlsTicketContents contents;
    ASSERT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", kNow + 1000, contents));
    EXPECT_EQ(TlsProtocolVersion::Tls13, contents.version);
    EXPECT_EQ(TlsCipherSuiteId::TlsAes128GcmSha256, contents.suite);
    EXPECT_EQ("h2", contents.alpn_view());
    EXPECT_EQ(0xdeadbeefu, contents.ticket_age_add);
    EXPECT_EQ(0u, contents.max_early_data);
    EXPECT_EQ(kNow, contents.issued_ms);
    EXPECT_EQ(3600u, contents.timeout_s);
    // The payload holds the DERIVED psk — the same Expand-Label the client
    // runs over the received NST — not the resumption master.
    const std::array<std::uint8_t, 1> nonce{0};
    const auto psk = tls13_resumption_psk(TlsSecret::from_bytes(fx.master13), nonce);
    ASSERT_TRUE(psk.has_value());
    EXPECT_TRUE(secrets_equal(*psk, contents.secret));
    EXPECT_FALSE(secrets_equal(TlsSecret::from_bytes(fx.master13), contents.secret));
}

TEST(TlsTicketService, Mint12RoundtripCarriesMaster) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request12());

    TlsTicketContents contents;
    ASSERT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", kNow + 1000, contents));
    EXPECT_EQ(TlsProtocolVersion::Tls12, contents.version);
    EXPECT_EQ(48u, contents.secret.len());
    EXPECT_TRUE(secrets_equal(TlsSecret::from_bytes(fx.master12), contents.secret));
    EXPECT_EQ("h2", contents.alpn_view());
    // 1.2 payload has no age/early-data fields; defaults ride through.
    EXPECT_EQ(0u, contents.ticket_age_add);
    EXPECT_EQ(0u, contents.max_early_data);
}

// Two mints of one request differ in nonce (fresh randomness per ticket), and
// both open.
TEST(TlsTicketService, DistinctNoncesDiffer) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> a = fx.mint(service, fx.request13());
    const std::vector<std::uint8_t> b = fx.mint(service, fx.request13());
    EXPECT_NE(a, b);
    TlsTicketContents ca;
    TlsTicketContents cb;
    ASSERT_EQ(TlsTicketService::OpenStatus::Ok, service.open(a, "example.com", kNow, ca));
    ASSERT_EQ(TlsTicketService::OpenStatus::Ok, service.open(b, "example.com", kNow, cb));
    EXPECT_TRUE(secrets_equal(ca.secret, cb.secret));
}

// ---- name binding ----

TEST(TlsTicketService, NameIsBoundIntoAad) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13("example.com"));

    TlsTicketContents contents;
    EXPECT_EQ(TlsTicketService::OpenStatus::Rejected, service.open(ticket, "other.example", kNow, contents));
    EXPECT_EQ(TlsTicketService::OpenStatus::Rejected, service.open(ticket, "", kNow, contents));
    ASSERT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", kNow, contents));

    // Empty-name round trip: empty == empty is the one allowed empty match.
    const std::vector<std::uint8_t> anon = fx.mint(service, fx.request13(""));
    EXPECT_EQ(TlsTicketService::OpenStatus::Ok, service.open(anon, "", kNow, contents));
    EXPECT_EQ(TlsTicketService::OpenStatus::Rejected, service.open(anon, "example.com", kNow, contents));
}

// ---- tamper / truncation ----

TEST(TlsTicketService, TamperedTicketRejected) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13());
    ASSERT_GT(ticket.size(), kHeaderLen + 16);

    // ver || key_id || nonce || ciphertext || tag — flip one bit per region.
    const std::vector<std::size_t> offsets{
            0, // container version
            1, // key_id
            5, // nonce
            kHeaderLen + 2, // ciphertext body
            ticket.size() - 1, // tag
    };
    for (const std::size_t off: offsets) {
        std::vector<std::uint8_t> bad = ticket;
        ASSERT_LT(off, bad.size());
        bad[off] ^= 0xa5;
        TlsTicketContents contents;
        EXPECT_EQ(TlsTicketService::OpenStatus::Rejected, service.open(bad, "example.com", kNow, contents))
                << "byte " << off << " survived";
    }
}

TEST(TlsTicketService, TruncatedTicketRejected) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13());
    for (std::size_t len = 0; len < ticket.size(); ++len) {
        TlsTicketContents contents;
        EXPECT_EQ(TlsTicketService::OpenStatus::Rejected,
                  service.open({ticket.data(), len}, "example.com", kNow, contents))
                << "prefix " << len << " opened";
    }
}

// ---- key windows (injected, immutable) ----

// [current, successor] injected up front: minting hands over exactly at the
// successor's birth time; the predecessor keeps opening through retention,
// then drops.
TEST(TlsTicketService, RotationHandsOverAtBirthAndRetainsThenDrops) {
    Fixture fx;
    TlsTicketKeyPolicy policy;
    policy.key_lifetime_s = 10;
    policy.key_retention_s = 10;
    const std::array<TlsTicketKeyMaterial, 2> keys{{
            {.id = 1, .bytes = fx.key_bytes, .created_ms = 0}, // mint [0, 10s), open [0, 20s)
            {.id = 2, .bytes = fx.key_bytes2, .created_ms = 10'000}, // mint [10s, 20s), open [10s, 30s)
    }};
    TlsTicketService service(keys, policy);
    ASSERT_TRUE(service.valid());
    EXPECT_EQ(2u, service.key_count());

    // Before the successor is born, the current key mints.
    const std::vector<std::uint8_t> t1 = fx.mint(service, fx.request13("example.com", 3600, 0));
    EXPECT_EQ(1u, key_id_of(t1));
    // At the successor's birth the pick flips — no future-key minting early.
    const std::vector<std::uint8_t> t2 = fx.mint(service, fx.request13("example.com", 3600, 10'000));
    EXPECT_EQ(2u, key_id_of(t2));
    EXPECT_NE(t1, t2);

    TlsTicketContents c;
    EXPECT_EQ(TlsTicketService::OpenStatus::Ok, service.open(t1, "example.com", 15'000, c)); // retained window
    EXPECT_EQ(TlsTicketService::OpenStatus::Ok, service.open(t2, "example.com", 21'000, c));
    EXPECT_EQ(TlsTicketService::OpenStatus::Rejected, service.open(t1, "example.com", 20'000, c)); // retention closed
}

// Once every key's mint window has passed, minting declines — no NST, never
// a wrong-key ticket. The assembly rotates by injecting fresh material.
TEST(TlsTicketService, MintStopsWhenAllKeysPastWindow) {
    Fixture fx;
    TlsTicketKeyPolicy policy;
    policy.key_lifetime_s = 10;
    const std::array<TlsTicketKeyMaterial, 1> keys{fx.keys(0)};
    TlsTicketService service(keys, policy);
    ASSERT_TRUE(service.valid());
    const std::vector<std::uint8_t> early = fx.mint(service, fx.request13("example.com", 3600, 5'000));
    EXPECT_FALSE(early.empty());
    EXPECT_EQ(0u, TlsTicketService::mint_thunk(&service, fx.request13("example.com", 3600, 20'000), fx.scratch));
}

// ---- expiry ----

TEST(TlsTicketService, TimeoutExpiryIsExpiredNotRejected) {
    Fixture fx;
    TlsTicketService service(fx.keys(0), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13("example.com", 5, 0));

    TlsTicketContents c;
    EXPECT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", 4'999, c));
    EXPECT_EQ(TlsTicketService::OpenStatus::Expired, service.open(ticket, "example.com", 5'001, c));
    // Forward clock drift (issued in the "future") is tolerated, not fatal.
    EXPECT_EQ(TlsTicketService::OpenStatus::Ok, service.open(ticket, "example.com", 1'000, c));
}

// ---- declined mints ----

TEST(TlsTicketService, BadInputsMintNothing) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    ASSERT_TRUE(service.valid());

    const auto mint_len = [&service, &fx](const TlsTicketRequest &req, std::size_t cap) {
        std::array<std::uint8_t, TlsTicketService::kMaxTicketLen> out{};
        return TlsTicketService::mint_thunk(&service, req, {out.data(), cap});
    };

    // 1.2 master must be exactly 48 bytes.
    TlsTicketRequest bad12 = fx.request12();
    const std::array<std::uint8_t, 47> short_master{};
    bad12.resumption_master = short_master;
    EXPECT_EQ(0u, mint_len(bad12, fx.scratch.size()));

    // 1.3 master must be a hash-length secret.
    TlsTicketRequest bad13 = fx.request13();
    bad13.resumption_master = short_master;
    EXPECT_EQ(0u, mint_len(bad13, fx.scratch.size()));

    // SNI beyond the wire bound.
    TlsTicketRequest bad_name = fx.request13();
    const std::string long_name(TlsTicketService::kMaxNameLen + 1, 'a');
    bad_name.name = long_name;
    EXPECT_EQ(0u, mint_len(bad_name, fx.scratch.size()));

    // ALPN beyond the length-prefix bound.
    TlsTicketRequest bad_alpn = fx.request13();
    const std::string long_alpn(256, 'x');
    bad_alpn.alpn = long_alpn;
    EXPECT_EQ(0u, mint_len(bad_alpn, fx.scratch.size()));

    // An unhandled version.
    TlsTicketRequest bad_version = fx.request13();
    bad_version.version = TlsProtocolVersion::Tls11;
    EXPECT_EQ(0u, mint_len(bad_version, fx.scratch.size()));

    // An out buffer too small for the container.
    EXPECT_EQ(0u, mint_len(fx.request13(), 16));
}

// ---- lookup thunk (the engine-facing resumption half) ----

TEST(TlsTicketService, LookupThunkResumes13Ticket) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request13());

    const TlsResumptionLookup lookup = service.lookup();
    ASSERT_NE(nullptr, lookup.lookup);
    TlsResumedSession resumed;
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, {}, resumed));
    EXPECT_EQ(TlsProtocolVersion::Tls13, resumed.version); // the 1.3 engine's gate input
    // The engine consumes the psk view synchronously — it must be the sealed
    // pre-derived PSK, i.e. exactly what the client derives from the NST.
    const std::array<std::uint8_t, 1> nonce{0};
    const auto psk = tls13_resumption_psk(TlsSecret::from_bytes(fx.master13), nonce);
    ASSERT_TRUE(psk.has_value());
    EXPECT_EQ(0, std::memcmp(psk->bytes().data(), resumed.psk.data(), psk->len()));
    EXPECT_EQ(psk->len(), resumed.psk.size());
    EXPECT_EQ(TlsCipherSuiteId::TlsAes128GcmSha256, resumed.suite);
    EXPECT_EQ("h2", resumed.alpn);
    EXPECT_EQ(0xdeadbeefu, resumed.ticket_age_add);
    EXPECT_EQ(0u, resumed.max_early_data);
    EXPECT_EQ(kNow, resumed.ticket_issued_ms);
}

// The hook is version-blind: a 1.2 ticket maps straight through with the
// master as `psk` and its own version — the 1.2 engine's abbreviated gates
// (and the 1.3 engine's version gate) consume exactly this shape.
TEST(TlsTicketService, LookupThunkResumes12TicketWithMaster) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::vector<std::uint8_t> ticket = fx.mint(service, fx.request12());

    const TlsResumptionLookup lookup = service.lookup();
    TlsResumedSession resumed;
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, {}, resumed));
    EXPECT_EQ(TlsProtocolVersion::Tls12, resumed.version);
    ASSERT_EQ(fx.master12.size(), resumed.psk.size());
    EXPECT_EQ(0, std::memcmp(fx.master12.data(), resumed.psk.data(), fx.master12.size()));
    EXPECT_EQ(TlsCipherSuiteId::TlsAes128GcmSha256, resumed.suite);
    EXPECT_EQ("h2", resumed.alpn);
    EXPECT_EQ(kNow, resumed.ticket_issued_ms);
}

// Every non-Ok open is a miss (false): wrong vhost name, tamper, and session
// expiry all fall back to a full handshake.
TEST(TlsTicketService, LookupThunkMissesOnWrongNameTamperOrExpiry) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const TlsResumptionLookup lookup = service.lookup();
    TlsResumedSession resumed;

    // Wrong vhost name — the AAD binding rejects before any decryption.
    const std::vector<std::uint8_t> other = fx.mint(service, fx.request13("other.example"));
    EXPECT_FALSE(lookup.lookup(lookup.ctx, other, "example.com", kNow, {}, resumed));

    // Tampered ciphertext.
    std::vector<std::uint8_t> bad = fx.mint(service, fx.request13());
    bad[kHeaderLen + 2] ^= 0xA5;
    EXPECT_FALSE(lookup.lookup(lookup.ctx, bad, "example.com", kNow, {}, resumed));

    // Session timeout: a 5 s ticket offered 6 s later (the key window is the
    // default 24 h, so this isolates the payload's own timeout).
    const std::vector<std::uint8_t> expired = fx.mint(service, fx.request13("example.com", 5, kNow));
    EXPECT_FALSE(lookup.lookup(lookup.ctx, expired, "example.com", kNow + 6'000, {}, resumed));

    // An invalid service (no keys) misses everything — never fatal: this very
    // ticket is unknown key material to it.
    TlsTicketService empty(std::span<const TlsTicketKeyMaterial>{}, TlsTicketKeyPolicy{});
    const TlsResumptionLookup empty_lookup = empty.lookup();
    EXPECT_FALSE(
            empty_lookup.lookup(empty_lookup.ctx, fx.mint(service, fx.request13()), "example.com", kNow, {}, resumed));
}

// ---- the 0-RTT consistency gate (10 §6.1) ----
// A QUIC-minted ticket carries the mint-time early-data context; the lookup
// compares it against the server's current one. A match keeps the ticket's
// early data; every mismatch (different bytes, different length, or the
// current context gone empty) demotes to 1-RTT resumption only — the
// session itself still resumes (lookup stays true).
TEST(TlsTicketService, LookupGateKeepsEarlyDataOnContextMatch) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::array<std::uint8_t, 5> context{{0x11, 0x22, 0x33, 0x44, 0x55}};
    TlsTicketRequest req = fx.request13();
    req.max_early_data = 0xffffffff; // the QUIC sentinel
    req.quic = true;
    req.quic_early_data_context = context;
    const std::vector<std::uint8_t> ticket = fx.mint(service, req);

    const TlsResumptionLookup lookup = service.lookup();
    TlsResumedSession resumed;
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, context, resumed));
    EXPECT_TRUE(resumed.quic); // the engine's face gate input
    EXPECT_EQ(0xffffffffu, resumed.max_early_data);
}

TEST(TlsTicketService, LookupGateVetoesEarlyDataOnContextMismatch) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    const std::array<std::uint8_t, 5> minted{{0x11, 0x22, 0x33, 0x44, 0x55}};
    const std::array<std::uint8_t, 5> altered{{0x11, 0x22, 0x33, 0x44, 0xAA}};
    const std::array<std::uint8_t, 4> shorter{{0x11, 0x22, 0x33, 0x44}};
    TlsTicketRequest req = fx.request13();
    req.max_early_data = 0xffffffff;
    req.quic = true;
    req.quic_early_data_context = minted;
    const std::vector<std::uint8_t> ticket = fx.mint(service, req);

    const TlsResumptionLookup lookup = service.lookup();
    TlsResumedSession resumed;
    // Same length, different bytes.
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, altered, resumed));
    EXPECT_TRUE(resumed.quic);
    EXPECT_EQ(0u, resumed.max_early_data); // 0-RTT vetoed, the session resumes
    // Different length.
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, shorter, resumed));
    EXPECT_EQ(0u, resumed.max_early_data);
    // The current context gone empty (early data since disabled).
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, {}, resumed));
    EXPECT_EQ(0u, resumed.max_early_data);
}

// A TCP-minted ticket has no gate to run: the context span is ignored and
// the early data it never had stays absent (the engine's face gate owns the
// cross-face miss).
TEST(TlsTicketService, LookupGateSkipsTcpMintedTickets) {
    Fixture fx;
    TlsTicketService service(fx.keys(), TlsTicketKeyPolicy{});
    TlsTicketRequest req = fx.request13();
    req.max_early_data = 14336; // the TCP budget
    const std::vector<std::uint8_t> ticket = fx.mint(service, req);

    const TlsResumptionLookup lookup = service.lookup();
    TlsResumedSession resumed;
    const std::array<std::uint8_t, 5> context{{0x11, 0x22, 0x33, 0x44, 0x55}};
    EXPECT_TRUE(lookup.lookup(lookup.ctx, ticket, "example.com", kNow, context, resumed));
    EXPECT_FALSE(resumed.quic);
    EXPECT_EQ(14336u, resumed.max_early_data);
}
