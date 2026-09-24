#ifndef FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H
#define FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../common/IoError.h"
#include "TlsHandshakeMessage.h"

namespace fiber::tls {

// ---- decode (server flight messages; 04 契约：borrow span、零分配、失败不动
// out、结构校验在此、语义在引擎) ----

// Decodes a ClientHello message body (the bytes AFTER the 4-byte handshake
// header; length == header.length). Structural validation only — version
// negotiation and policy checks belong to the server FSM. Fails with
// IoErr::Invalid on any RFC 8446 §4.1.2 / §4.2 malformation: field lengths,
// inexact list walks, trailing bytes, duplicate extensions, empty-payload
// violations, pre_shared_key not placed last, identity/binder count mismatch.
// `out` is only mutated on success.
[[nodiscard]] common::IoResult<void> tls_decode_client_hello(const std::uint8_t *body, std::size_t len,
                                                             TlsClientHello &out) noexcept;

// ServerHello body — SH and HelloRetryRequest share this wire form. Fails with
// IoErr::Invalid on field-length / exact-walk / duplicate-extension /
// wrong-payload-shape violations. HRR identification is the engine's call
// (tls_is_hello_retry_request); both the selected_group (empty key_exchange)
// and server-share (non-empty) key_share forms decode.
[[nodiscard]] common::IoResult<void> tls_decode_server_hello(const std::uint8_t *body, std::size_t len,
                                                             TlsServerHello &out) noexcept;

// EncryptedExtensions body. ALPN extracts the (single) selected protocol;
// early_data must be empty-payload. SH-exclusive extensions are NOT rejected
// here — that check is version-dependent and belongs to the engine.
[[nodiscard]] common::IoResult<void> tls_decode_encrypted_extensions(const std::uint8_t *body, std::size_t len,
                                                                     TlsEncryptedExtensions &out) noexcept;

// Certificate bodies. Empty certificate_list is structurally valid (a client
// may send no certificate); rejecting an empty SERVER chain is the engine's
// semantic rule. Entry count above the chain cap fails decode.
[[nodiscard]] common::IoResult<void> tls_decode_certificate_13(const std::uint8_t *body, std::size_t len,
                                                               TlsCertificate13 &out) noexcept;
[[nodiscard]] common::IoResult<void> tls_decode_certificate_12(const std::uint8_t *body, std::size_t len,
                                                               TlsCertificate12 &out) noexcept;

// CertificateRequest bodies. The 1.3 form FAILS decode when the
// signature_algorithms extension is missing (RFC 8446 §4.3.2 MUST); the 1.2
// form treats it as optional (RFC 5246 allows TLS 1.0 peers without it).
[[nodiscard]] common::IoResult<void> tls_decode_certificate_request_13(const std::uint8_t *body, std::size_t len,
                                                                       TlsCertificateRequest13 &out) noexcept;
[[nodiscard]] common::IoResult<void> tls_decode_certificate_request_12(const std::uint8_t *body, std::size_t len,
                                                                       TlsCertificateRequest12 &out) noexcept;

// CertificateVerify body: scheme + length-prefixed signature, exact walk.
[[nodiscard]] common::IoResult<void> tls_decode_certificate_verify(const std::uint8_t *body, std::size_t len,
                                                                   TlsCertificateVerify &out) noexcept;

// Finished body: verify_data spans the remainder (non-empty). Its expected
// LENGTH (1.2: 12 / 1.3: suite hash len) is the engine's check.
[[nodiscard]] common::IoResult<void> tls_decode_finished(const std::uint8_t *body, std::size_t len,
                                                         TlsFinished &out) noexcept;

// 1.2 ECDHE ServerKeyExchange: named_curve form only (curve_type 3) + scheme
// + signature, exact walk. Other curve forms fail decode.
[[nodiscard]] common::IoResult<void> tls_decode_server_key_exchange(const std::uint8_t *body, std::size_t len,
                                                                    TlsServerKeyExchange &out) noexcept;

// 1.2 ECDHE ClientKeyExchange body: the 1-byte-length-prefixed client point,
// exact walk. Point length/curve validity is the engine's + KX decap's check.
[[nodiscard]] common::IoResult<void> tls_decode_client_key_exchange(const std::uint8_t *body, std::size_t len,
                                                                    TlsClientKeyExchange &out) noexcept;

// ---- encode（06 契约：写入调用方 scratch，返回长度；分配归 context）----

// Everything the ClientHello encoder needs. All spans borrow caller data for
// the call only. Presence is span-driven: empty sni_host/alpn/session_ticket
// omits the extension; non-empty key_share emits exactly one share entry.
// supported_versions defaults to the engine-fixed [0x0304, 0x0303] and
// narrows to offered_versions (09 §4.2); psk_key_exchange_modes (psk_dhe_ke)
// is an engine-fixed constant, not an input.
struct TlsClientHelloInput {
    std::span<const std::uint8_t> random; // exactly 32
    std::span<const std::uint8_t> session_id; // 0..32
    std::span<const std::uint16_t> cipher_suites; // >= 1
    std::span<const std::uint16_t> supported_groups; // >= 1
    std::span<const std::uint16_t> signature_algorithms; // >= 1
    std::span<const std::uint16_t> offered_versions; // empty = [0x0304, 0x0303]
    std::uint16_t key_share_group = 0; // raw group id
    std::span<const std::uint8_t> key_share; // non-empty => key_share extension
    std::string_view sni_host; // non-empty => server_name
    std::span<const std::string_view> alpn; // non-empty => ALPN list
    std::span<const std::uint8_t> session_ticket; // non-empty => 1.2 RFC 5077 offer
    std::span<const std::uint8_t> cookie; // non-empty => cookie extension (HRR echo, RFC 8446 §4.2.2)
    bool offer_extended_master_secret = true;
    bool offer_renegotiation_info = true; // RFC 5746 empty initial RI
    bool has_psk = false; // pre_shared_key — always the LAST extension
    std::span<const std::uint8_t> psk_identity;
    std::uint32_t psk_obfuscated_ticket_age = 0;
    std::uint8_t psk_binder_len = 0; // psk suite hash length (32 | 48)
    bool early_data = false; // early_data extension (psk offers only)
    std::span<const std::uint8_t> quic_transport_params; // non-empty => 0x39, opaque passthrough (10 §5)
};

struct TlsClientHelloEncoded {
    std::size_t len = 0; // bytes written to scratch (message incl. header)
    // Offset from the MESSAGE start (incl. 4-byte header) of the binders
    // vector's 2-byte length prefix — the transcript truncation point for
    // binder recomputation and where the zeroed binder block begins.
    std::size_t binder_block_offset = 0;
};

// Exact encoded size of the message this input produces (pure; may itself
// fail Invalid on contract violations — same checks as the encoder).
[[nodiscard]] common::IoResult<std::size_t> tls_client_hello_size(const TlsClientHelloInput &in) noexcept;

// Encodes the complete handshake message (4-byte header + body) into scratch.
// Binder bytes are written ZERO; the engine computes the binder over the
// truncated transcript and backfills at binder_block_offset.
[[nodiscard]] common::IoResult<TlsClientHelloEncoded> tls_encode_client_hello(const TlsClientHelloInput &in,
                                                                              std::span<std::uint8_t> scratch) noexcept;

// Generic wrapper: 4-byte header(type, body.size()) + body copy. Empty body
// is valid (EndOfEarlyData; ServerHelloDone).
[[nodiscard]] common::IoResult<std::size_t> tls_encode_handshake_message(TlsHandshakeType type,
                                                                         std::span<const std::uint8_t> body,
                                                                         std::span<std::uint8_t> scratch) noexcept;

// Client Certificate. The 1.3 form echoes certificate_request_context (empty
// in the initial handshake); certs.size() above the chain cap fails Invalid.
[[nodiscard]] common::IoResult<std::size_t>
tls_encode_certificate_13(std::span<const std::uint8_t> request_context,
                          std::span<const std::span<const std::uint8_t>> certs,
                          std::span<std::uint8_t> scratch) noexcept;
[[nodiscard]] common::IoResult<std::size_t>
tls_encode_certificate_12(std::span<const std::span<const std::uint8_t>> certs,
                          std::span<std::uint8_t> scratch) noexcept;

// CertificateVerify: raw scheme + the signature the engine computed.
[[nodiscard]] common::IoResult<std::size_t> tls_encode_certificate_verify(std::uint16_t scheme,
                                                                          std::span<const std::uint8_t> signature,
                                                                          std::span<std::uint8_t> scratch) noexcept;

// Finished: verify_data copy (length is the engine's version/suite contract).
[[nodiscard]] common::IoResult<std::size_t> tls_encode_finished(std::span<const std::uint8_t> verify_data,
                                                                std::span<std::uint8_t> scratch) noexcept;

// 1.2 ECDHE ClientKeyExchange (RFC 4492 §5.7): the client's public point with
// its 1-byte length prefix — the curve comes from the server's SKE. point is
// the raw encoded public value (X25519: 32 bytes; P-256: 65 uncompressed).
[[nodiscard]] common::IoResult<std::size_t> tls_encode_client_key_exchange(std::span<const std::uint8_t> point,
                                                                           std::span<std::uint8_t> scratch) noexcept;

// ---- encode（07 补充：server flight）----

// Everything the ServerHello encoder needs; one struct covers the three wire
// forms. `tls13` selects the extension set:
//   true  — key_share (server share, or the HRR selected_group form when
//           key_share is empty; the HRR sentinel random arrives via `random`),
//           supported_versions (0x0304, engine-fixed), then pre_shared_key
//           LAST iff selected_identity (RFC 8446 §4.2.11 ordering);
//   false — the 1.2 set [extended_master_secret][renegotiation_info]
//           [alpn][session_ticket] in that order (BoringSSL kExtensions walk).
// legacy_version (0x0303) and compression (null) are fixed by both RFCs.
struct TlsServerHelloInput {
    std::span<const std::uint8_t> random; // exactly 32 (HRR: kTlsHelloRetryRandom)
    std::span<const std::uint8_t> session_id; // 0..32 (echo the CH's)
    std::uint16_t cipher_suite = 0; // raw wire value
    bool tls13 = false;
    // 1.3 fields:
    std::uint16_t key_share_group = 0; // raw group id
    std::span<const std::uint8_t> key_share; // non-empty => server share
    bool selected_identity = false; // pre_shared_key echo (always LAST)
    std::uint16_t identity = 0;
    // 1.2 fields:
    bool extended_master_secret = false; // echo iff the CH offered
    bool renegotiation_info = true; // always on for 1.2 (empty vector)
    std::string_view alpn; // non-empty => selected protocol
    bool session_ticket = false; // RFC 5077 echo (always the empty-payload form)
};

[[nodiscard]] common::IoResult<std::size_t> tls_encode_server_hello(const TlsServerHelloInput &in,
                                                                    std::span<std::uint8_t> scratch) noexcept;

// EncryptedExtensions: [server_name echo][alpn][early_data] in that order.
// All presence-driven; server_name acks the CH's SNI with an EMPTY extension
// (RFC 8446 §4.2.1 — no ServerNameList; a payload is malformed at 1.3).
struct TlsEncryptedExtensionsInput {
    bool acknowledge_server_name = false;
    std::string_view alpn; // non-empty => selected protocol
    bool early_data = false; // empty extension (0-RTT accepted)
    std::span<const std::uint8_t> quic_transport_params; // non-empty => 0x39, opaque passthrough (10 §5)
};

[[nodiscard]] common::IoResult<std::size_t> tls_encode_encrypted_extensions(const TlsEncryptedExtensionsInput &in,
                                                                            std::span<std::uint8_t> scratch) noexcept;

// CertificateRequest. The 1.3 form: 1-byte context length 0 + an extension
// block with exactly signature_algorithms. The 1.2 form (RFC 5246 §7.4.4):
// certificate_types {rsa_sign(1), ecdsa_sign(64)} (engine-fixed) + the u16
// sigalgs vector + an EMPTY certificate_authorities vector (u16 length 0 —
// any CA acceptable; the u24 form died with RFC 2246). sigalgs must be
// non-empty in both forms.
[[nodiscard]] common::IoResult<std::size_t>
tls_encode_certificate_request_13(std::span<const std::uint16_t> signature_algorithms,
                                  std::span<std::uint8_t> scratch) noexcept;
[[nodiscard]] common::IoResult<std::size_t>
tls_encode_certificate_request_12(std::span<const std::uint16_t> signature_algorithms,
                                  std::span<std::uint8_t> scratch) noexcept;

// 1.2 ECDHE ServerKeyExchange: curve_type(3, fixed) + group + the 1-byte
// length-prefixed server point + scheme + signature. The engine signs
// client_random ‖ server_random ‖ ServerECDHParams separately; this encoder
// only frames.
struct TlsServerKeyExchangeInput {
    std::uint16_t named_group = 0; // raw
    std::span<const std::uint8_t> public_key; // 1..255 (raw point)
    std::uint16_t scheme = 0; // raw
    std::span<const std::uint8_t> signature; // non-empty
};

[[nodiscard]] common::IoResult<std::size_t> tls_encode_server_key_exchange(const TlsServerKeyExchangeInput &in,
                                                                           std::span<std::uint8_t> scratch) noexcept;

// NewSessionTicket. 1.3 (RFC 8446 §4.6.1): u32 lifetime + u32 ticket_age_add
// + 1-byte-length nonce (the engine-fixed sequence number, so always 1 byte)
// + u16 ticket + extensions {[early_data: u32 max_early_data]} — the
// extension is omitted when max_early_data == 0. 1.2 (RFC 5077 §3.3):
// u32 lifetime + u16 ticket, sent in cleartext before the server CCS.
struct TlsNewSessionTicket13Input {
    std::uint32_t lifetime_s = 0;
    std::uint32_t ticket_age_add = 0;
    std::uint8_t ticket_nonce = 0; // the sequence number
    std::span<const std::uint8_t> ticket; // 1..65535
    std::uint32_t max_early_data = 0; // 0 => omit the early_data extension
};

[[nodiscard]] common::IoResult<std::size_t> tls_encode_new_session_ticket_13(const TlsNewSessionTicket13Input &in,
                                                                             std::span<std::uint8_t> scratch) noexcept;
[[nodiscard]] common::IoResult<std::size_t> tls_encode_new_session_ticket_12(std::uint32_t lifetime_s,
                                                                             std::span<const std::uint8_t> ticket,
                                                                             std::span<std::uint8_t> scratch) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H
