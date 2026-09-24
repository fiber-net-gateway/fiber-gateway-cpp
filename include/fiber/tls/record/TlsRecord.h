#ifndef FIBER_TLS_RECORD_TLS_RECORD_H
#define FIBER_TLS_RECORD_TLS_RECORD_H

#include <cstddef>
#include <cstdint>
#include <optional>

#include "../../common/Assert.h"
#include "../../common/mem/IoBuf.h"
#include "../../common/mem/IoBufChain.h"

namespace fiber::tls {

enum class TlsContentType : std::uint8_t {
    ChangeCipherSpec = 20,
    Alert = 21,
    Handshake = 22,
    ApplicationData = 23,
};

inline constexpr std::size_t kTlsRecordHeaderSize = 5;

// RFC 8446 §5.1 / RFC 5246 §6.2.1 fragment bounds.
inline constexpr std::size_t kTlsMaxPlaintextSize = 1 << 14;
// AEAD ciphertext overhead bounds: RFC 5246 §6.2.3.3 allows 2^14 + 2048, RFC
// 8446 §5.2 allows 2^14 + 256. The framing layer enforces the larger (1.2)
// cap; the exact per-version check belongs to the record protection layer.
inline constexpr std::size_t kTlsMaxCiphertextOverhead12 = 2048;
inline constexpr std::size_t kTlsMaxCiphertextOverhead13 = 256;
inline constexpr std::size_t kTlsMaxCiphertextRecordSize = kTlsMaxPlaintextSize + kTlsMaxCiphertextOverhead12;

// legacy_record_version values seen on the wire. RFC 8446 §5.1: the field MUST
// be ignored for all purposes; it is parsed and exposed but never rejected.
inline constexpr std::uint16_t kTlsRecordVersionTls10 = 0x0301;
inline constexpr std::uint16_t kTlsRecordVersionTls11 = 0x0302;
inline constexpr std::uint16_t kTlsRecordVersionTls12 = 0x0303;

[[nodiscard]] bool tls_is_valid_content_type(std::uint8_t type) noexcept;

struct TlsRecordHeader {
    TlsContentType type = TlsContentType::ApplicationData;
    std::uint16_t legacy_version = 0;
    std::uint16_t length = 0;
};

// Decodes the 5-byte record header from `src`. Disengaged marks an unknown
// content type (connection-fatal, unexpected_message).
[[nodiscard]] std::optional<TlsRecordHeader> tls_decode_record_header(const std::uint8_t *src) noexcept;

// Encodes the 5-byte record header (inverse of tls_decode_record_header).
inline void tls_encode_record_header(std::uint8_t *dst, TlsContentType type, std::uint16_t version,
                                     std::uint16_t length) noexcept {
    dst[0] = static_cast<std::uint8_t>(type);
    dst[1] = static_cast<std::uint8_t>(version >> 8);
    dst[2] = static_cast<std::uint8_t>(version & 0xff);
    dst[3] = static_cast<std::uint8_t>(length >> 8);
    dst[4] = static_cast<std::uint8_t>(length & 0xff);
}

// One inbound record: the raw bytes as they arrived from the transport, split
// at the record boundary. The payload is TAKEN from the reader's input chain
// (whole-node moves plus zero-copy boundary slices; the payload itself is
// never copied), so the record owns its bytes independently of the reader.
//
// Lifetime contract: chain nodes resolve the current loop's node pool —
// destroy the record on the connection's loop. Bytes leaving the connection
// (pass-up to other threads) must be re-minted as plain IoBuf slices, whose
// storage lifetime is refcounted.
struct TlsRecord {
    TlsContentType type = TlsContentType::ApplicationData;
    std::uint16_t legacy_version = 0;
    // == payload.readable_bytes(); carried separately so consumers can dispatch
    // without touching the chain. Zero-length records have no payload span.
    std::uint16_t length = 0;
    mem::IoBufChain payload{};

    // Single contiguous span covering the whole payload, or nullptr when the
    // record straddles chain nodes (or is empty). The common case — a record
    // contained in one transport read — is contiguous; consumers needing
    // linear bytes take this fast path and copy otherwise.
    [[nodiscard]] const std::uint8_t *contiguous_payload() const noexcept {
        if (length == 0) {
            return nullptr;
        }
        FIBER_ASSERT(payload.readable_bytes() == length);
        const mem::IoBuf *front = payload.first_readable();
        if (front == nullptr || front->readable() != length) {
            return nullptr;
        }
        return front->readable_data();
    }
};

} // namespace fiber::tls

#endif // FIBER_TLS_RECORD_TLS_RECORD_H
