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

// ---- encode（06 契约：写入调用方 scratch，返回长度；分配归 context）----

// Everything the ClientHello encoder needs. All spans borrow caller data for
// the call only. Presence is span-driven: empty sni_host/alpn/session_ticket
// omits the extension; non-empty key_share emits exactly one share entry.
// supported_versions ([0x0304, 0x0303]) and psk_key_exchange_modes
// (psk_dhe_ke) are engine-fixed constants, not inputs.
struct TlsClientHelloInput {
    std::span<const std::uint8_t> random; // exactly 32
    std::span<const std::uint8_t> session_id; // 0..32
    std::span<const std::uint16_t> cipher_suites; // >= 1
    std::span<const std::uint16_t> supported_groups; // >= 1
    std::span<const std::uint16_t> signature_algorithms; // >= 1
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

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_CODEC_H
