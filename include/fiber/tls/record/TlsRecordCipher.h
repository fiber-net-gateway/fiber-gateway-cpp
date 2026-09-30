#ifndef FIBER_TLS_RECORD_TLS_RECORD_CIPHER_H
#define FIBER_TLS_RECORD_TLS_RECORD_CIPHER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <openssl/aead.h>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../handshake/TlsCipherSuites.h"
#include "TlsRecord.h"

namespace fiber::tls {

// Which record-protection construction the instance applies. TLS 1.3
// (RFC 8446 §5.2) and TLS 1.2 AEAD differ in nonce derivation, AAD and
// payload framing; within 1.2 the two AEADs ALSO differ in the nonce form
// (RFC 5288 GCM: 4-byte fixed IV + 8-byte explicit nonce on the wire;
// RFC 7905 §2 ChaCha20: a 12-byte implicit IV, none of it on the wire), and
// the legacy CBC suites (feature/tls/11) are a third 1.2 form: a fresh random
// 16-byte IV on the wire ahead of the CBC body, an 11-byte AD (the EVP adds
// the length itself) and a length-dependent MAC + padding overhead.
// The primitive itself is absorbed by EVP_AEAD_CTX — BoringSSL's TLS CBC
// AEADs included (MAC-then-encrypt, constant-time padding/MAC check).
enum class TlsRecordProtectionKind : std::uint8_t { Tls13, Tls12 };

// Which way an instance protects records. Every instance serves exactly one
// direction (see below); the 1.2 CBC AEADs additionally bind it at init
// (EVP_AEAD_CTX_init_with_direction), the AEAD suites ignore it.
enum class TlsRecordDirection : std::uint8_t { Seal, Open };

// Record-protection primitive: raw key material + byte ranges in, protected
// / restored byte ranges out. One instance per DIRECTION per key epoch
// (RFC 8446 §5.2 / RFC 5246 §6.2.3.3): the suite, key and iv are frozen at
// init, the instance owns its sequence number starting at 0, and every key
// change — 1.2 ChangeCipherSpec, the three 1.3 traffic secrets, KeyUpdate —
// is expressed by the engine swapping in a fresh instance. The cipher has no
// state machine and never asks which epoch it is in.
//
// No per-record allocation: no node pool, no IoBufChain, spans only. The one
// internal construction, EVP_AEAD_CTX_init, is an in-struct fixed-size key
// schedule for all three AEADs; the CBC AEADs allocate their HMAC_CTX and
// cipher state there too (twice per instance, at init only). Buffer
// orchestration (gather, in-place view adjustments, chain building) lives in
// the engine (feature/tls/05 §5.3).
//
// Aliasing (in-place) contract, per operation — anything else that overlaps
// FIBER_ASSERTs; disjoint dst never copies except the one 1.3 case below:
//   seal 1.3   dst.data() == plaintext.data()      (inner-type byte written
//              into dst at plaintext_len — may be past the plaintext span
//              but inside dst's capacity, i.e. the node's tailroom)
//   seal 1.2   dst.data() + explicit_nonce_len() == plaintext.data() (the
//              GCM explicit nonce / CBC IV at dst's head; 0 for ChaCha, so
//              in-place)
//   open 1.3   dst.data() == ciphertext.data()
//   open 1.2   dst.data() == ciphertext.data() + explicit_nonce_len()
//              (the plaintext always lands at dst.data(); ChaCha in-place;
//              CBC's dst capacity also covers the MAC + padding it strips)
// With a disjoint dst, seal 1.3 performs the only memcpy (the EVP takes one
// input region, so the inner-type byte is appended first); seal/open 1.2 and
// open 1.3 are moved natively by the EVP (out != in, no overlap).
//
// After protection starts, record headers are always 0x0303 on the wire
// (1.3 by spec; 1.2 post-CCS conventional — the engine resets the writer's
// legacy version), so both constructions hardcode 0x0303 in the AAD/outer
// version; open takes the received header fields verbatim for AAD instead.
// Movable (not copyable): the handshake engines hand their live traffic
// ciphers to TlsConnectedState by move — a byte-steal plus a zeroing of the
// source, valid because a zeroed EVP_AEAD_CTX is uninitialized and the moved-
// from instance flags itself uninitialized (destructor becomes a no-op). The
// sequence number travels with the instance, so record-protection continuity
// across the handshake→connection phase boundary is structural.
class TlsRecordCipher : public common::NonCopyable {
public:
    TlsRecordCipher() noexcept = default;
    TlsRecordCipher(TlsRecordCipher &&other) noexcept;
    TlsRecordCipher &operator=(TlsRecordCipher &&other) noexcept;
    ~TlsRecordCipher();

    enum class Status : std::uint8_t {
        Ok,
        AuthFail, // AEAD authentication (CBC: MAC or padding, indistinguishably) failed -> bad_record_mac
        Malformed, // length out of bounds (CBC: or not whole blocks), wrong outer type, bad/missing inner type
        Overflow, // authenticated plaintext over the limit -> record_overflow (RFC 8446 §5.4, RFC 5246 §6.2.1)
    };

    struct SealResult {
        Status status = Status::Ok;
        std::size_t out_len = 0; // == seal_output_size(plaintext.size()) on Ok
    };

    struct OpenResult {
        Status status = Status::Ok;
        TlsContentType inner_type = TlsContentType::ApplicationData;
        std::size_t plain_len = 0; // actual plaintext length (1.3: padding stripped)
    };

    // Freezes (suite, kind, direction, key, iv). The key material is copied
    // into the EVP_AEAD_CTX; the caller may drop the source spans right after.
    // Fails with IoErr::Invalid when the suite/kind pairing is not one of the
    // implemented combinations or the key/iv length mismatches. Single-shot:
    // re-init of a live instance is a contract violation.
    [[nodiscard]] common::IoResult<void> init(TlsCipherSuiteId suite, TlsRecordProtectionKind kind,
                                              TlsRecordDirection direction, std::span<const std::uint8_t> key,
                                              std::span<const std::uint8_t> iv) noexcept;

    // ---- size computation (pure; callers size their buffers from these) ----

    // Capacity dst must have for seal: exact overhead, plaintext + 17 (1.3:
    // inner type + tag) or + 16 + explicit_nonce_len() (1.2: tag, plus the
    // GCM explicit nonce); 1.2 CBC: 16 (IV) + roundup(plaintext + MAC + 1, 16).
    [[nodiscard]] std::size_t seal_output_size(std::size_t plaintext_len) const noexcept;
    // Capacity dst must have for open. 1.2 AEAD is exact
    // (ciphertext_len - 16 - explicit_nonce_len()). 1.3 is a capacity
    // requirement of ciphertext_len - 16 — the EVP output workspace includes
    // the inner type and zero padding, which are then stripped; the actual
    // plaintext is OpenResult::plain_len. 1.2 CBC likewise: ciphertext_len -
    // 16 (everything past the IV), the MAC and padding stripped after.
    [[nodiscard]] std::size_t open_output_size(std::size_t ciphertext_len) const noexcept;
    // Pre-decryption ciphertext length bounds, checked before any crypto.
    [[nodiscard]] std::size_t min_ciphertext_size() const noexcept;
    [[nodiscard]] std::size_t max_ciphertext_size() const noexcept;

    // ---- transforms ----

    // Plaintext in, protected record payload out (nonce prefix and tag
    // included; the record header is NOT). inner_type: 1.3 encrypts it as
    // the trailing content-type byte; 1.2 binds it into the AAD. On Ok the
    // sequence number advances. A dst capacity below seal_output_size is a
    // contract violation (FIBER_ASSERT), as is any partial overlap.
    [[nodiscard]] SealResult seal(TlsContentType inner_type, std::span<const std::uint8_t> plaintext,
                                  std::span<std::uint8_t> dst) noexcept;

    // Protected record payload in (explicit nonce + tag included), plaintext
    // out at dst.data(). outer_type/legacy_version/length are the RECEIVED
    // record header: 1.3 binds them verbatim into the AAD (and requires
    // outer_type == ApplicationData); 1.2 binds them plus this instance's
    // sequence number. Length bounds are checked BEFORE decryption (no
    // decrypt oracle). On Ok the sequence number advances.
    [[nodiscard]] OpenResult open(TlsContentType outer_type, std::uint16_t legacy_version, std::uint16_t length,
                                  std::span<const std::uint8_t> ciphertext, std::span<std::uint8_t> dst) noexcept;

    // ---- scatter forms (chain-friendly: the tag lives in its own span) ----
    //
    // CBC has no detachable tag on open (BoringSSL's TLS CBC AEADs implement
    // no open_gather): its tag span is EMPTY and the body covers everything
    // past the IV. On seal, dst_tag receives the encrypted MAC tail + padding
    // (seal_output_size(n) - 16 - n bytes, at most 48) and dst_prefix the IV.

    // seal with the output split across three spans so an in-place chain
    // transform never touches node headroom/tailroom: ciphertext body to
    // dst_ct (in-place allowed: dst_ct.data() == plaintext.data(), length
    // preserved), the AEAD tag to dst_tag, and per kind 1.3 the encrypted
    // inner-type byte in front of the tag (dst_tag receives 17 bytes,
    // ct(type)||tag, routed through the EVP's extra_in — no adjacency between
    // the type byte and the plaintext is needed) / 1.2 the explicit nonce to
    // dst_prefix (dst_prefix must be EMPTY for 1.3). dst_prefix/dst_tag must
    // not alias any other span (EVP requirement). out_len on Ok ==
    // seal_output_size(plaintext.size()); the wire payload is
    // dst_prefix || dst_ct || dst_tag[0 .. tag_len]. On EVP failure the
    // touched outputs are zeroed by the library.
    [[nodiscard]] SealResult seal_scatter(TlsContentType inner_type, std::span<const std::uint8_t> plaintext,
                                          std::span<std::uint8_t> dst_ct, std::span<std::uint8_t> dst_prefix,
                                          std::span<std::uint8_t> dst_tag) noexcept;

    // open with the input split: ciphertext body and tag as separate spans
    // (the tag may live in another chain node — verified against BoringSSL)
    // and 1.2's explicit nonce from its own read-only span (EMPTY for 1.3).
    // In-place dst.data() == body.data() allowed; partial overlap
    // FIBER_ASSERTs. Unlike open(), the plaintext always lands at dst.data()
    // with no +8 offset — the nonce is not part of dst. Same length bounds /
    // AAD / padding-strip / sequence semantics as open(). On EVP failure dst's
    // body region is zeroed by the library.
    [[nodiscard]] OpenResult open_scatter(TlsContentType outer_type, std::uint16_t legacy_version, std::uint16_t length,
                                          std::span<const std::uint8_t> explicit_nonce,
                                          std::span<const std::uint8_t> body, std::span<const std::uint8_t> tag,
                                          std::span<std::uint8_t> dst) noexcept;

    // ---- queries ----

    // The 1.2 wire nonce: 8 explicit bytes for GCM suites (RFC 5288), 0 for
    // ChaCha20-Poly1305 (RFC 7905 §2 implicit IV), 16 for CBC (the per-record
    // random IV). Always 0 at 1.3.
    [[nodiscard]] std::size_t explicit_nonce_len() const noexcept { return explicit_nonce_len_; }
    // A 1.2 CBC suite (MAC-then-encrypt, feature/tls/11).
    [[nodiscard]] bool is_cbc() const noexcept { return mac_len_ != 0; }
    // The tag the chain adapter splits off a record's tail: the 16-byte AEAD
    // tag, or 0 for CBC (its MAC and padding sit inside the CBC body).
    [[nodiscard]] std::size_t detached_tag_len() const noexcept { return is_cbc() ? 0 : 16; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return seq_; }
    [[nodiscard]] TlsCipherSuiteId suite() const noexcept { return suite_; }
    [[nodiscard]] TlsRecordProtectionKind kind() const noexcept { return kind_; }
    [[nodiscard]] TlsRecordDirection direction() const noexcept { return direction_; }
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

private:
    void move_from(TlsRecordCipher &src) noexcept;

    EVP_AEAD_CTX aead_ctx_{}; // zeroed == uninitialized; cleanup-safe
    TlsCipherSuiteId suite_ = TlsCipherSuiteId::TlsAes128GcmSha256;
    TlsRecordProtectionKind kind_ = TlsRecordProtectionKind::Tls13;
    TlsRecordDirection direction_ = TlsRecordDirection::Seal;
    std::array<std::uint8_t, 12> iv_{}; // 1.3 static iv (12) / 1.2 iv (GCM 4, ChaCha 12, CBC none)
    std::uint8_t explicit_nonce_len_ = 0; // 1.2 GCM: 8; 1.2 CBC: 16; 1.3 & 1.2 ChaCha: 0
    std::uint8_t mac_len_ = 0; // 1.2 CBC: 20 (SHA-1) | 32 (SHA-256); 0 = an AEAD suite
    std::uint64_t seq_ = 0;
    bool initialized_ = false;
};

} // namespace fiber::tls

#endif // FIBER_TLS_RECORD_TLS_RECORD_CIPHER_H
