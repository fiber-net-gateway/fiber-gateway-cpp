#ifndef FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_MESSAGE_H
#define FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_MESSAGE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../common/IoError.h"

namespace fiber::tls {

// Handshake message types, complete for TLS 1.2 (RFC 5246 §7.4) + TLS 1.3
// (RFC 8446 §4). Unassigned wire values (7, 9, 10, 17..19, ...) still decode
// into the enum via static_cast — dispatch simply won't match a known type.
enum class TlsHandshakeType : std::uint8_t {
    HelloRequest = 0, // 1.2 renegotiation trigger; we answer NO_RENEGOTIATION
    ClientHello = 1,
    ServerHello = 2,
    HelloVerifyRequest = 3, // DTLS-only (RFC 6347 §4.2.1); TCP stack never sees it
    NewSessionTicket = 4,
    EndOfEarlyData = 5, // 1.3
    HelloRetryRequest = 6, // 1.3; transcript swaps in MessageHash(254)
    EncryptedExtensions = 8, // 1.2: RFC 7301 ALPN 选择; 1.3: 全部服务端扩展
    Certificate = 11,
    ServerKeyExchange = 12, // 1.2 ECDHE 参数 + 签名
    CertificateRequest = 13,
    ServerHelloDone = 14, // 1.2 server flight 终止符
    CertificateVerify = 15,
    ClientKeyExchange = 16, // 1.2 client ECDHE share / premaster
    Finished = 20,
    CertificateUrl = 21, // 1.2 客户端认证变体，罕见
    CertificateStatus = 22, // OCSP stapling
    SupplementalData = 23,
    KeyUpdate = 24, // 1.3
    CompressedCertificate = 25, // RFC 8879，仅协商 compress_certificate 时出现
    NextProtocol = 67, // Google NPN，已被 ALPN 取代；仅登记
    MessageHash = 254, // 1.3 HRR 转录占位
};

inline constexpr std::size_t kTlsHandshakeHeaderSize = 4;
inline constexpr std::uint32_t kTlsMaxHandshakeMessageSize = (1U << 24) - 1;

struct TlsHandshakeHeader {
    TlsHandshakeType type = TlsHandshakeType::ClientHello;
    std::uint32_t length = 0; // body length, excludes the 4-byte header
};

// Decodes the 4-byte handshake header. Disengaged: truncated.
[[nodiscard]] common::IoResult<TlsHandshakeHeader> tls_decode_handshake_header(const std::uint8_t *src,
                                                                               std::size_t len) noexcept;

// Decoded ClientHello. All spans borrow the message body buffer passed to
// tls_decode_client_hello; the caller owns that buffer for the struct's life.
struct TlsClientHello {
    std::uint16_t legacy_version = 0;
    std::span<const std::uint8_t> random{}; // exactly 32 bytes
    std::span<const std::uint8_t> session_id{}; // 0..32
    std::span<const std::uint8_t> cipher_suites{}; // even count, >= 2 bytes
    std::span<const std::uint8_t> compression_methods{}; // >= 1 byte
    // Raw extension block (the extensions vector body, without its 2-byte
    // length) for transcript hashing and rescans.
    std::span<const std::uint8_t> extensions_block{};

    // server_name (RFC 6066): first host_name entry, bytes as sent.
    bool has_server_name = false;
    std::string_view server_name{};

    // supported_versions: raw u16 list (CH variant, 1-byte list length).
    bool has_supported_versions = false;
    std::span<const std::uint8_t> supported_versions{};

    // supported_groups / signature_algorithms[_cert]: raw u16 lists.
    bool has_supported_groups = false;
    std::span<const std::uint8_t> supported_groups{};
    bool has_signature_algorithms = false;
    std::span<const std::uint8_t> signature_algorithms{};
    bool has_signature_algorithms_cert = false;
    std::span<const std::uint8_t> signature_algorithms_cert{};

    // key_share: client_shares list body (entries: group(2), kx-len(2), kx).
    bool has_key_share = false;
    std::span<const std::uint8_t> key_share_entries{};

    // ALPN: ProtocolNameList body.
    bool has_alpn = false;
    std::span<const std::uint8_t> alpn_list{};

    // psk_key_exchange_modes: mode bytes.
    bool has_psk_key_exchange_modes = false;
    std::span<const std::uint8_t> psk_key_exchange_modes{};

    bool has_early_data = false;

    // session_ticket (RFC 5077, TLS 1.2 resumption): opaque ticket.
    bool has_session_ticket = false;
    std::span<const std::uint8_t> session_ticket{};

    // record_size_limit (RFC 8449): exactly 2-byte value.
    bool has_record_size_limit = false;
    std::uint16_t record_size_limit = 0;

    // Flag-only extensions.
    bool has_encrypt_then_mac = false;
    bool has_extended_master_secret = false;
    bool has_renegotiation_info = false;

    // pre_shared_key (RFC 8446 §4.2.11). Structurally validated; binder
    // verification is the server FSM's job. psk_binder_block_offset counts
    // from the message BODY start to the binders vector's 2-byte length
    // prefix — the transcript truncation point for binder recomputation
    // (add kTlsHandshakeHeaderSize for the message-relative offset).
    bool has_pre_shared_key = false;
    std::span<const std::uint8_t> psk_identities{};
    std::span<const std::uint8_t> psk_binders{};
    std::size_t psk_binder_block_offset = 0;
    std::uint16_t psk_identity_count = 0;
    std::uint16_t psk_binder_count = 0;
};

// ---- ServerHello (RFC 8446 §4.1.3 / RFC 5246 §7.4.1.2). HelloRetryRequest
// shares this wire form; distinguish by the sentinel random below. All spans
// borrow the message body passed to tls_decode_server_hello. ----
struct TlsServerHello {
    std::uint16_t legacy_version = 0;
    std::span<const std::uint8_t> random{}; // exactly 32 bytes
    std::span<const std::uint8_t> session_id{}; // 0..32
    std::uint16_t cipher_suite = 0; // raw wire value
    std::uint8_t compression_method = 0;
    std::span<const std::uint8_t> extensions_block{};

    // supported_versions, SH variant: a single 2-byte value (no list wrapper).
    bool has_supported_version = false;
    std::uint16_t supported_version = 0;

    // key_share, SH/HRR variant: one entry. In a real SH the key_exchange is
    // the server's share (non-empty); in an HRR it is the selected_group form
    // with an EMPTY key_exchange. The engine picks by HRR-vs-SH.
    bool has_key_share = false;
    std::uint16_t key_share_group = 0; // raw group value
    std::span<const std::uint8_t> key_share{};

    bool has_cookie = false; // HRR only: opaque payload
    std::span<const std::uint8_t> cookie{};

    bool has_selected_identity = false; // PSK resumption accepted
    std::uint16_t selected_identity = 0;

    bool has_alpn = false; // 1.2 SH-selected protocol (RFC 7301); 1.3 puts ALPN in EE
    std::string_view alpn{};

    bool has_extended_master_secret = false; // 1.2 echo (empty payload)
    bool has_renegotiation_info = false; // 1.2 echo; content kept for the engine's empty-check
    std::span<const std::uint8_t> renegotiation_info{};
};

// HelloRetryRequest sentinel random (RFC 8446 §4.1.3): an SH carrying exactly
// this value IS an HRR (handshake type 6 semantics) regardless of the wire type byte.
inline constexpr std::array<std::uint8_t, 32> kTlsHelloRetryRandom = {
        0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C, 0x02, 0x1E, 0x65, 0xB8, 0x91,
        0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB, 0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C};

[[nodiscard]] inline bool tls_is_hello_retry_request(std::span<const std::uint8_t> random) noexcept {
    if (random.size() != 32) {
        return false;
    }
    for (std::size_t i = 0; i < 32; ++i) {
        if (random.data()[i] != kTlsHelloRetryRandom[i]) {
            return false;
        }
    }
    return true;
}

// ---- EncryptedExtensions (RFC 8446 §4.3.1): extension block + the two
// values the client engine consumes. ----
struct TlsEncryptedExtensions {
    std::span<const std::uint8_t> extensions_block{};
    bool has_alpn = false;
    std::string_view alpn{};
    bool has_early_data = false;
};

// ---- Certificate (1.3 form: RFC 8446 §4.4.2; 1.2 form: RFC 5246 §7.4.2).
// Entry DER spans borrow the message body. The 1.3 per-entry extension
// vectors are structurally walked but not extracted (we negotiate no cert
// extensions). ----
struct TlsCertificate13 {
    static constexpr std::size_t kMaxEntries = 4; // parity with the 02b chain cap
    std::string_view certificate_request_context{}; // empty in the initial handshake
    std::span<const std::uint8_t> certs[kMaxEntries]{};
    std::size_t cert_count = 0;
};

struct TlsCertificate12 {
    static constexpr std::size_t kMaxEntries = 4;
    std::span<const std::uint8_t> certs[kMaxEntries]{};
    std::size_t cert_count = 0;
};

// ---- CertificateRequest. The 1.3 form is a context + extension block (the
// signature_algorithms extension is REQUIRED — enforced by the decoder); the
// 1.2 form is three plain vectors (types, optional sigalgs, DN list). ----
struct TlsCertificateRequest13 {
    std::string_view certificate_request_context{};
    std::span<const std::uint8_t> extensions_block{};
    bool has_signature_algorithms = false;
    std::span<const std::uint8_t> signature_algorithms{}; // raw u16 list
};

struct TlsCertificateRequest12 {
    std::span<const std::uint8_t> certificate_types{}; // raw 1-byte list
    bool has_signature_algorithms = false;
    std::span<const std::uint8_t> signature_algorithms{}; // raw u16 list
    std::span<const std::uint8_t> certificate_authorities{}; // raw DN list (not parsed)
};

// ---- CertificateVerify / Finished (both versions share the forms; the
// verify_data LENGTH check is the engine's — version/suite dependent). ----
struct TlsCertificateVerify {
    std::uint16_t algorithm = 0; // raw signature scheme
    std::span<const std::uint8_t> signature{};
};

struct TlsFinished {
    std::span<const std::uint8_t> verify_data{}; // 1.2: 12; 1.3: suite hash len
};

// ---- 1.2 ECDHE ServerKeyExchange (RFC 4492 §5.4 shape): named-curve params
// plus signature. Explicit/prime curve forms are not parsed — curve_type 3 is
// the only structurally accepted form (others fail decode; the engine maps
// that to illegal_parameter). ----
struct TlsServerKeyExchange {
    std::uint8_t curve_type = 0; // 3 = named_curve
    std::uint16_t named_group = 0; // raw
    std::span<const std::uint8_t> public_key{}; // the point (1-byte length prefix on the wire)
    std::uint16_t algorithm = 0; // raw scheme
    std::span<const std::uint8_t> signature{};
};

// ---- 1.2 ECDHE ClientKeyExchange (RFC 4492 §5.7): the client's public point
// with its 1-byte length prefix; the curve is whatever the server's SKE
// named. Length/encoding validity is the engine's + KX decap's check. ----
struct TlsClientKeyExchange {
    std::span<const std::uint8_t> public_key{};
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_HANDSHAKE_MESSAGE_H
