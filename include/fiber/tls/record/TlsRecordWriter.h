#ifndef FIBER_TLS_RECORD_TLS_RECORD_WRITER_H
#define FIBER_TLS_RECORD_TLS_RECORD_WRITER_H

#include <cstddef>
#include <cstdint>

#include "../../common/IoError.h"
#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/IoBuf.h"
#include "../../common/mem/IoBufChain.h"
#include "TlsRecord.h"

namespace fiber::tls {

// Outbound record framer: plaintext in, TLS records out (RFC 8446 §5.1 /
// RFC 5246 §6.2.1) — the inverse of TlsRecordReader.
//   - payloads larger than kTlsMaxPlaintextSize are split into a sequence of
//     records of the same content type (handshake message fragmentation);
//   - every write emits at least one record: an empty payload produces a
//     zero-length record (legal for application data; the FSM avoids asking
//     for it elsewhere);
//   - framing is TAKE, never copy: whole payload nodes are spliced into the
//     output chain unchanged, a node straddling a record boundary is split by
//     a zero-copy retained view. Per record the only fresh allocation is one
//     5-byte header IoBuf;
//   - legacy_record_version comes from the stored value (default 0x0303; a
//     client may set 0x0301 for its first flight — receivers must ignore the
//     field either way).
// Record protection (AEAD, sequence numbers) is a later layer's job: this
// writer emits plaintext records only. Pure memory: no fds, no coroutines,
// no openssl.
//
// Node pool contract: chains resolve the current loop's node pool per
// operation (see IoBufChain) — every write runs on the connection's loop.
class TlsRecordWriter : public common::NonCopyable, public common::NonMovable {
public:
    TlsRecordWriter() noexcept = default;

    void set_legacy_version(std::uint16_t version) noexcept { legacy_version_ = version; }
    [[nodiscard]] std::uint16_t legacy_version() const noexcept { return legacy_version_; }

    // Appends the framed records to `out` and drains `payload`. `out` must be
    // pool-bound. Fails only with IoErr::NoMem; on failure `out` may hold a
    // prefix of the framing — callers treat NoMem as fatal.
    [[nodiscard]] common::IoResult<void> write(TlsContentType type, mem::IoBuf &&payload,
                                               mem::IoBufChain &out) noexcept;
    [[nodiscard]] common::IoResult<void> write(TlsContentType type, mem::IoBufChain &&payload,
                                               mem::IoBufChain &out) noexcept;

private:
    [[nodiscard]] common::IoResult<void> write_header(TlsContentType type, std::size_t length,
                                                      mem::IoBufChain &out) noexcept;

    std::uint16_t legacy_version_ = kTlsRecordVersionTls12;
};

} // namespace fiber::tls

#endif // FIBER_TLS_RECORD_TLS_RECORD_WRITER_H
