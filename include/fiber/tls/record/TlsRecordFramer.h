#ifndef FIBER_TLS_RECORD_TLS_RECORD_FRAMER_H
#define FIBER_TLS_RECORD_TLS_RECORD_FRAMER_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "../TlsTypes.h"
#include "TlsRecord.h"

namespace fiber::tls {

// Connected-phase record framing over ONE contiguous wire region (feature/tls/12
// §3): the glue keeps every inbound record inside a single buffer, so a framed
// record is just a descriptor — no chain, no node, no copy — and the
// connection opens it in place. The framing rules match TlsRecordReader (the
// chain-based splitter the handshake keeps using):
//   - unknown content types are connection-fatal (unexpected_message);
//   - lengths above the universal ciphertext cap are connection-fatal
//     (record_overflow) as soon as the header is in, without waiting for the
//     body; per-version caps are the record protection layer's;
//   - legacy_record_version is parsed but never rejected;
//   - zero-length payloads pass through.

// One framed record: its payload is region[offset, offset + length), right
// behind its 5-byte header.
struct TlsRecordSpan {
    std::uint32_t offset = 0;
    std::uint16_t length = 0;
    std::uint16_t legacy_version = 0;
    TlsContentType type = TlsContentType::ApplicationData;
};

// Records per framing call: the glue's on-stack batch.
inline constexpr std::size_t kTlsRecordBatchMax = 32;

struct TlsFrameResult {
    std::size_t count = 0; // records written to `out`
    std::size_t consumed = 0; // wire bytes those records cover, headers included
    // A framing violation right behind the `count` records: the caller
    // processes those first, then raises `alert`.
    bool fatal = false;
    TlsAlertDesc alert = TlsAlertDesc::CloseNotify;
};

// Frames complete records off the head of `wire` into `out`, in wire order.
// Stops at the first incomplete record (header or body), when `out` is full,
// or at a framing violation. Pure: no allocation, `wire` is only read.
[[nodiscard]] TlsFrameResult tls_frame_records(std::span<const std::uint8_t> wire,
                                               std::span<TlsRecordSpan> out) noexcept;

} // namespace fiber::tls

#endif // FIBER_TLS_RECORD_TLS_RECORD_FRAMER_H
