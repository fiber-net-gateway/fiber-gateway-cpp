#include <fiber/tls/TlsTicketService.h>

// Stateless ticket minting/opening (feature/tls/08 §3). The AEAD is
// EVP_AEAD (the same primitive the record cipher uses) and entropy comes
// through TlsCryptoPrimitives. The header carries the ring's EVP_AEAD_CTX
// by value (the TlsRecordCipher.h precedent); entropy and the schedule
// derivation stay behind this TU. The key set is injected and immutable —
// no lock anywhere (see the header's key-management note).

#include <cstring>
#include <utility>

#include <fiber/tls/crypto/Tls13KeySchedule.h>
#include "../crypto/TlsCryptoPrimitives.h"

namespace fiber::tls {

namespace {

// ---- container constants (version 2; see the header for the layout) ----

constexpr std::uint8_t kContainerVersion = 2; // v2: the 1.3 payload gained the QUIC face + early-data context
constexpr std::size_t kContainerHeaderLen = 1 + 4 + 12; // ver || key_id || nonce
constexpr std::size_t kTagLen = 16;
constexpr std::uint8_t kKindTls13 = 0x13;
constexpr std::uint8_t kKindTls12 = 0x12;
constexpr std::size_t kAeadKeyLen = 16;
constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kMaxAlpnLen = 255;
constexpr std::size_t kMaxPayloadLen = 512; // 1+1+48+2+1+255+4+4+8+4 = 328; +2+128 context, headroom
constexpr std::size_t kMaxAadLen = 1 + 4 + 2 + TlsTicketService::kMaxNameLen;

void store_be16(std::uint8_t *dst, std::uint16_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v >> 8);
    dst[1] = static_cast<std::uint8_t>(v);
}

void store_be32(std::uint8_t *dst, std::uint32_t v) noexcept {
    dst[0] = static_cast<std::uint8_t>(v >> 24);
    dst[1] = static_cast<std::uint8_t>(v >> 16);
    dst[2] = static_cast<std::uint8_t>(v >> 8);
    dst[3] = static_cast<std::uint8_t>(v);
}

void store_be64(std::uint8_t *dst, std::uint64_t v) noexcept {
    for (int i = 7; i >= 0; --i) {
        dst[i] = static_cast<std::uint8_t>(v);
        v >>= 8;
    }
}

[[nodiscard]] std::uint16_t load_be16(const std::uint8_t *src) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[0]) << 8) | src[1]);
}

[[nodiscard]] std::uint32_t load_be32(const std::uint8_t *src) noexcept {
    return (static_cast<std::uint32_t>(src[0]) << 24) | (static_cast<std::uint32_t>(src[1]) << 16) |
           (static_cast<std::uint32_t>(src[2]) << 8) | static_cast<std::uint32_t>(src[3]);
}

[[nodiscard]] std::uint64_t load_be64(const std::uint8_t *src) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | src[i];
    }
    return v;
}

// Bounded big-endian writer/reader pair over stack buffers; every append or
// read past the bound fails the whole operation (no partial tickets).
struct Writer {
    std::array<std::uint8_t, kMaxPayloadLen> buf{};
    std::size_t n = 0;

    [[nodiscard]] bool u8(std::uint8_t v) noexcept {
        if (n >= buf.size()) {
            return false;
        }
        buf[n++] = v;
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t v) noexcept {
        if (n + 2 > buf.size()) {
            return false;
        }
        store_be16(buf.data() + n, v);
        n += 2;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t v) noexcept {
        if (n + 4 > buf.size()) {
            return false;
        }
        store_be32(buf.data() + n, v);
        n += 4;
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t v) noexcept {
        if (n + 8 > buf.size()) {
            return false;
        }
        store_be64(buf.data() + n, v);
        n += 8;
        return true;
    }
    [[nodiscard]] bool bytes(std::span<const std::uint8_t> v) noexcept {
        if (n + v.size() > buf.size()) {
            return false;
        }
        std::memcpy(buf.data() + n, v.data(), v.size());
        n += v.size();
        return true;
    }
};

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t off = 0;

    [[nodiscard]] bool u8(std::uint8_t &out) noexcept {
        if (off + 1 > data.size()) {
            return false;
        }
        out = data[off++];
        return true;
    }
    [[nodiscard]] bool u16(std::uint16_t &out) noexcept {
        if (off + 2 > data.size()) {
            return false;
        }
        out = load_be16(data.data() + off);
        off += 2;
        return true;
    }
    [[nodiscard]] bool u32(std::uint32_t &out) noexcept {
        if (off + 4 > data.size()) {
            return false;
        }
        out = load_be32(data.data() + off);
        off += 4;
        return true;
    }
    [[nodiscard]] bool u64(std::uint64_t &out) noexcept {
        if (off + 8 > data.size()) {
            return false;
        }
        out = load_be64(data.data() + off);
        off += 8;
        return true;
    }
    [[nodiscard]] bool bytes(std::uint8_t *dst, std::size_t len) noexcept {
        if (off + len > data.size()) {
            return false;
        }
        std::memcpy(dst, data.data() + off, len);
        off += len;
        return true;
    }
    [[nodiscard]] bool empty() const noexcept { return off == data.size(); }
};

// The mint-side payload: the kind byte selects the field set, so one
// container serves both versions.
[[nodiscard]] bool build_payload(Writer &w, const TlsTicketRequest &req,
                                 std::span<const std::uint8_t> secret) noexcept {
    const bool tls13 = req.version == TlsProtocolVersion::Tls13;
    if (!w.u8(tls13 ? kKindTls13 : kKindTls12) || !w.u8(static_cast<std::uint8_t>(secret.size())) || !w.bytes(secret) ||
        !w.u16(static_cast<std::uint16_t>(req.suite)) || !w.u8(static_cast<std::uint8_t>(req.alpn.size())) ||
        !w.bytes({reinterpret_cast<const std::uint8_t *>(req.alpn.data()), req.alpn.size()})) {
        return false;
    }
    if (tls13 && (!w.u32(req.ticket_age_add) || !w.u32(req.max_early_data))) {
        return false;
    }
    if (!w.u64(static_cast<std::uint64_t>(req.now_unix_ms)) || !w.u32(req.timeout_s)) {
        return false;
    }
    // 1.3 only (v2): the QUIC face + the 0-RTT consistency gate's mint-time
    // context. 1.2 never runs on QUIC, so its payload keeps the v1 shape.
    if (tls13 && (!w.u8(req.quic ? 1 : 0) || !w.u8(static_cast<std::uint8_t>(req.quic_early_data_context.size())) ||
                  !w.bytes(req.quic_early_data_context))) {
        return false;
    }
    return true;
}

// AAD = ver || key_id || be16(name.len) || name. The name rides OUTSIDE the
// ciphertext so the cross-vhost binding is checkable before any decryption.
[[nodiscard]] std::size_t build_aad(std::array<std::uint8_t, kMaxAadLen> &aad, std::uint32_t key_id,
                                    std::string_view name) noexcept {
    aad[0] = kContainerVersion;
    store_be32(aad.data() + 1, key_id);
    store_be16(aad.data() + 5, static_cast<std::uint16_t>(name.size()));
    std::memcpy(aad.data() + 7, name.data(), name.size());
    return 7 + name.size();
}

} // namespace

// =====================================================================
// Key ring — injected once, immutable afterwards (ring members are direct;
// no indirection, no allocation)
// =====================================================================

const TlsTicketService::Key *TlsTicketService::mint_key(std::int64_t now_ms) const noexcept {
    for (std::size_t i = key_count_; i-- > 0;) {
        if (now_ms >= keys_[i].created_at_ms && now_ms < keys_[i].retire_at_ms) {
            return &keys_[i];
        }
    }
    return nullptr; // every key is past its mint window: NSTs stop until
                    // the assembly swaps in fresh material — no ticket,
                    // no resumption, never a wrong-key ticket
}

const TlsTicketService::Key *TlsTicketService::find_key(std::uint32_t id, std::int64_t now_ms) const noexcept {
    for (std::size_t i = 0; i < key_count_; ++i) {
        if (keys_[i].id == id) {
            return now_ms < keys_[i].drop_at_ms ? &keys_[i] : nullptr;
        }
    }
    return nullptr;
}

// =====================================================================
// Lifecycle
// =====================================================================

TlsTicketService::TlsTicketService(std::span<const TlsTicketKeyMaterial> keys,
                                   const TlsTicketKeyPolicy &policy) noexcept {
    if (keys.empty()) {
        return; // valid() false; the minter declines every mint
    }
    // Degenerate policy normalizes (a zero lifetime would retire at birth).
    TlsTicketKeyPolicy sane = policy;
    if (sane.key_lifetime_s == 0) {
        sane.key_lifetime_s = 1;
    }

    // Keep the kMaxKeys newest by created_ms, ordered oldest-first (capped
    // insertion, stable on ties, no allocation). Overflow drops the OLDEST
    // keys — their tickets stop opening early, the documented safe
    // degradation.
    std::array<const TlsTicketKeyMaterial *, kMaxKeys> picked{};
    std::size_t picked_n = 0;
    for (const TlsTicketKeyMaterial &k: keys) {
        if (picked_n == picked.size()) {
            if (k.created_ms <= picked[0]->created_ms) {
                continue; // older than (or tied with) every kept key: drop
            }
            for (std::size_t i = 1; i < picked.size(); ++i) {
                picked[i - 1] = picked[i]; // evict the oldest, open the tail slot
            }
            --picked_n;
        }
        std::size_t pos = picked_n;
        while (pos > 0 && picked[pos - 1]->created_ms > k.created_ms) {
            --pos;
        }
        for (std::size_t i = picked_n; i > pos; --i) {
            picked[i] = picked[i - 1];
        }
        picked[pos] = &k;
        ++picked_n;
    }

    // Duplicate ids would make open() ambiguous — stay invalid instead.
    for (std::size_t i = 0; i < picked_n; ++i) {
        for (std::size_t j = i + 1; j < picked_n; ++j) {
            if (picked[i]->id == picked[j]->id) {
                return;
            }
        }
    }

    for (std::size_t i = 0; i < picked_n; ++i) {
        const TlsTicketKeyMaterial &k = *picked[i];
        Key &slot = keys_[key_count_];
        if (EVP_AEAD_CTX_init(&slot.aead, EVP_aead_aes_128_gcm(), k.bytes.data(), kAeadKeyLen,
                              EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr) != 1) {
            for (std::size_t j = 0; j < key_count_; ++j) {
                EVP_AEAD_CTX_cleanup(&keys_[j].aead); // zeroizes key material
            }
            key_count_ = 0;
            return;
        }
        slot.id = k.id;
        slot.created_at_ms = k.created_ms;
        slot.retire_at_ms = k.created_ms + static_cast<std::int64_t>(sane.key_lifetime_s) * 1000;
        slot.drop_at_ms = slot.retire_at_ms + static_cast<std::int64_t>(sane.key_retention_s) * 1000;
        ++key_count_;
    }
}

TlsTicketService::~TlsTicketService() {
    // The ring is immutable, so no synchronization question exists here:
    // destruction is single-owner by contract (the glue tears the service
    // down on its own loop, after the engines that borrowed it).
    for (std::size_t i = 0; i < key_count_; ++i) {
        EVP_AEAD_CTX_cleanup(&keys_[i].aead); // zeroizes the key material
    }
}

bool TlsTicketService::random_key(std::uint32_t id, std::int64_t created_ms, TlsTicketKeyMaterial &out) noexcept {
    out.id = id;
    out.created_ms = created_ms;
    return tls_random_bytes(out.bytes);
}

TlsTicketMinter TlsTicketService::minter() const noexcept {
    // The hook ABI is a void* ctx; the service is immutable, so the const is
    // only re-added inside the thunk.
    return TlsTicketMinter{&mint_thunk, const_cast<TlsTicketService *>(this)};
}

std::size_t TlsTicketService::key_count() const noexcept { return key_count_; }

// =====================================================================
// Mint
// =====================================================================

std::size_t TlsTicketService::mint_thunk(void *ctx, const TlsTicketRequest &req, std::span<std::uint8_t> out) noexcept {
    auto &self = *static_cast<TlsTicketService *>(ctx);
    if (self.key_count_ == 0) {
        return 0; // construction failed — no NST this connection
    }

    // ---- input validation (a declined mint is always safe) ----
    if (req.name.size() > kMaxNameLen || req.alpn.size() > kMaxAlpnLen ||
        req.quic_early_data_context.size() > TlsTicketContents::kMaxQuicContextLen) {
        return 0;
    }
    // 1.3: the ticket carries the DERIVED PSK, not the resumption master —
    // the same Expand-Label(resumption_master, "resumption", nonce) the
    // client performs on the received NST, so both sides arrive at one key
    // with the master never leaving the mint moment.
    TlsSecret secret{};
    if (req.version == TlsProtocolVersion::Tls13) {
        if (req.resumption_master.size() != 32 && req.resumption_master.size() != 48) {
            return 0; // schedule contract guard (hash-len secrets only)
        }
        const std::array<std::uint8_t, 1> nonce{req.ticket_nonce};
        auto psk = tls13_resumption_psk(TlsSecret::from_bytes(req.resumption_master), nonce);
        if (!psk.has_value()) {
            return 0;
        }
        secret = std::move(psk).value();
    } else if (req.version == TlsProtocolVersion::Tls12) {
        if (req.resumption_master.size() != 48) {
            return 0; // the 1.2 master secret is a fixed 48 bytes
        }
        secret = TlsSecret::from_bytes(req.resumption_master);
    } else {
        return 0;
    }

    Writer payload;
    if (!build_payload(payload, req, secret.bytes())) {
        return 0;
    }
    if (out.size() < kContainerHeaderLen + payload.n + kTagLen) {
        tls_secure_wipe(payload.buf.data(), payload.n);
        return 0;
    }

    // Lock-free: the ring never changes after construction.
    const Key *key = self.mint_key(req.now_unix_ms);
    if (key == nullptr) {
        tls_secure_wipe(payload.buf.data(), payload.n);
        return 0; // no key in its mint window — minting pauses (header note)
    }

    std::array<std::uint8_t, kNonceLen> nonce{};
    if (!tls_random_bytes(nonce)) {
        tls_secure_wipe(payload.buf.data(), payload.n);
        return 0;
    }
    std::array<std::uint8_t, kMaxAadLen> aad{};
    const std::size_t aad_len = build_aad(aad, key->id, req.name);

    out[0] = kContainerVersion;
    store_be32(out.data() + 1, key->id);
    std::memcpy(out.data() + 5, nonce.data(), kNonceLen);
    std::size_t written = 0;
    const int sealed =
            EVP_AEAD_CTX_seal(&key->aead, out.data() + kContainerHeaderLen, &written, out.size() - kContainerHeaderLen,
                              nonce.data(), kNonceLen, payload.buf.data(), payload.n, aad.data(), aad_len);
    tls_secure_wipe(payload.buf.data(), payload.n); // held the psk/master
    return sealed == 1 ? kContainerHeaderLen + written : 0;
}

// =====================================================================
// Open (the resumption slice's container codec)
// =====================================================================

TlsTicketService::OpenStatus TlsTicketService::open(std::span<const std::uint8_t> ticket, std::string_view name,
                                                    std::int64_t now_unix_ms, TlsTicketContents &out) const noexcept {
    if (ticket.size() < kContainerHeaderLen + 2 + kTagLen || ticket.size() > kMaxTicketLen ||
        ticket[0] != kContainerVersion || name.size() > kMaxNameLen) {
        return OpenStatus::Rejected;
    }
    const std::uint32_t key_id = load_be32(ticket.data() + 1);
    const std::span<const std::uint8_t> nonce{ticket.data() + 5, kNonceLen};
    const std::span<const std::uint8_t> sealed{ticket.data() + kContainerHeaderLen,
                                               ticket.size() - kContainerHeaderLen};

    // Lock-free: the ring is immutable. key_count_ == 0 simply finds nothing.
    const Key *key = find_key(key_id, now_unix_ms);
    if (key == nullptr) {
        return OpenStatus::Rejected; // unknown key, or retention closed
    }

    std::array<std::uint8_t, kMaxAadLen> aad{};
    const std::size_t aad_len = build_aad(aad, key_id, name);
    std::array<std::uint8_t, kMaxPayloadLen> plain{};
    std::size_t plain_len = 0;
    const int opened = EVP_AEAD_CTX_open(&key->aead, plain.data(), &plain_len, plain.size(), nonce.data(), kNonceLen,
                                         sealed.data(), sealed.size(), aad.data(), aad_len);
    if (opened != 1) {
        return OpenStatus::Rejected; // tamper, wrong name, wrong key — no distinction
    }

    Reader r{{plain.data(), plain_len}};
    std::uint8_t kind = 0;
    std::uint8_t secret_len = 0;
    if (!r.u8(kind) || !r.u8(secret_len) || secret_len == 0 || secret_len > TlsSecret::kMaxLen) {
        return OpenStatus::Rejected;
    }
    TlsTicketContents contents;
    if (kind == kKindTls13) {
        contents.version = TlsProtocolVersion::Tls13;
    } else if (kind == kKindTls12) {
        if (secret_len != 48) {
            return OpenStatus::Rejected;
        }
        contents.version = TlsProtocolVersion::Tls12;
    } else {
        return OpenStatus::Rejected;
    }
    std::array<std::uint8_t, TlsSecret::kMaxLen> secret{};
    std::uint16_t suite = 0;
    std::uint8_t alpn_len = 0;
    std::uint32_t timeout_s = 0;
    std::uint64_t issued_ms = 0;
    if (!r.bytes(secret.data(), secret_len) || !r.u16(suite) || !r.u8(alpn_len) || alpn_len > kMaxAlpnLen ||
        !r.bytes(contents.alpn.data(), alpn_len)) {
        return OpenStatus::Rejected;
    }
    contents.alpn_len = alpn_len;
    if (kind == kKindTls13 && (!r.u32(contents.ticket_age_add) || !r.u32(contents.max_early_data))) {
        return OpenStatus::Rejected;
    }
    if (!r.u64(issued_ms) || !r.u32(timeout_s)) {
        return OpenStatus::Rejected;
    }
    if (kind == kKindTls13) {
        // v2 tail: QUIC face + the sealed early-data context. A v1 ticket
        // (no tail) fails the u8 read — Rejected, a full handshake next time.
        std::uint8_t quic = 0;
        std::uint8_t ctx_len = 0;
        if (!r.u8(quic) || !r.u8(ctx_len) || ctx_len > TlsTicketContents::kMaxQuicContextLen ||
            !r.bytes(contents.quic_context.data(), ctx_len)) {
            return OpenStatus::Rejected;
        }
        contents.quic = quic != 0;
        contents.quic_context_len = ctx_len;
    }
    if (!r.empty()) {
        return OpenStatus::Rejected; // trailing bytes = not our format
    }
    contents.secret = TlsSecret::from_bytes({secret.data(), secret_len});
    tls_secure_wipe(secret.data(), secret_len);
    tls_secure_wipe(plain.data(), plain_len);
    contents.suite = static_cast<TlsCipherSuiteId>(suite);
    contents.issued_ms = static_cast<std::int64_t>(issued_ms);
    contents.timeout_s = timeout_s;

    // Freshness: a negative elapsed time is forward clock drift — tolerated
    // (the deployment's skew, not a forgery signal); a past-timeout ticket
    // is the session expiring, not an authentication failure.
    const std::int64_t elapsed_ms = now_unix_ms - contents.issued_ms;
    if (elapsed_ms > static_cast<std::int64_t>(timeout_s) * 1000) {
        return OpenStatus::Expired;
    }
    out = std::move(contents);
    return OpenStatus::Ok;
}

// =====================================================================
// Lookup — the engine-facing resumption half (08 §7)
// =====================================================================

TlsResumptionLookup TlsTicketService::lookup() const noexcept {
    return TlsResumptionLookup{&lookup_thunk, const_cast<TlsTicketService *>(this)};
}

// The returned spans must outlive the hook's frame — the engine reads them
// right AFTER the call returns — so the opened ticket parks in this
// thread-local staging cell (an engine consumes its lookup result within the
// same handshake step; the same thread's next lookup overwrites the cell).
thread_local TlsTicketContents t_staged_resumption;

// A miss (false) is always safe: the engine drops the resumption offer and
// runs a full handshake. Rejected and Expired opens land here. The hook is
// version-blind: both ticket kinds map straight through (`version` says
// which payload it was; each engine rejects the other version's ticket).
bool TlsTicketService::lookup_thunk(void *ctx, std::span<const std::uint8_t> identity, std::string_view name,
                                    std::int64_t now_unix_ms, std::span<const std::uint8_t> quic_early_data_context,
                                    TlsResumedSession &out) noexcept {
    auto &self = *static_cast<TlsTicketService *>(ctx);
    TlsTicketContents &contents = t_staged_resumption;
    if (self.open(identity, name, now_unix_ms, contents) != OpenStatus::Ok) {
        return false;
    }
    out.psk = contents.secret.bytes(); // 1.3: the derived PSK; 1.2: the master
    out.version = contents.version;
    out.suite = contents.suite;
    out.alpn = contents.alpn_view();
    out.ticket_age_add = contents.ticket_age_add;
    out.max_early_data = contents.max_early_data;
    out.ticket_issued_ms = contents.issued_ms;
    out.quic = contents.quic;
    // The 0-RTT consistency gate (10 §6.1) — BoringSSL quic_ticket_compatible
    // parity: a QUIC-minted ticket's early data survives only when the
    // current context equals the mint-time one (an empty stored context
    // never matches, exactly as upstream). The veto demotes the ticket to
    // 1-RTT resumption only; the engine's `max_early_data > 0` test does
    // the downgrade. A TCP-minted ticket carries no context to compare.
    if (contents.quic &&
        !tls_constant_time_equal({contents.quic_context.data(), contents.quic_context_len}, quic_early_data_context)) {
        out.max_early_data = 0;
    }
    return true;
}

} // namespace fiber::tls
