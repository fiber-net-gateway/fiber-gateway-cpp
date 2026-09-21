#ifndef FIBER_TLS_RECORD_TLS_RECORD_READER_H
#define FIBER_TLS_RECORD_TLS_RECORD_READER_H

#include <cstddef>
#include <cstdint>

#include "../../common/NonCopyable.h"
#include "../../common/NonMovable.h"
#include "../../common/mem/IoBuf.h"
#include "../../common/mem/IoBufChain.h"
#include "../TlsTypes.h"
#include "TlsRecord.h"

namespace fiber::tls {

// Streaming record splitter over an inbound byte stream: feeds of
// IoBuf/IoBufChain bytes in, whole TLS records out. Owns the framing rules
// that are independent of cipher state (RFC 8446 §5.1 / RFC 5246 §6.2.1):
//   - unknown content types are connection-fatal (unexpected_message);
//   - lengths above the universal ciphertext cap are connection-fatal
//     (record_overflow); per-version caps are the record protection layer's;
//   - legacy_record_version is parsed but never rejected;
//   - zero-length payloads pass through; type-specific rules (CCS records
//     carry exactly one byte, handshake reassembly tolerates empty fragments)
//     belong to the consumers.
// Splitting is TAKE, never copy: the record's bytes are moved out of the
// buffered input (whole nodes spliced, boundary nodes split by zero-copy
// retained views). Materializing a straddling record into contiguous form is
// the consumers' job (TlsRecord::contiguous_payload() is the fast path).
// Pure memory: no fds, no coroutines, no openssl.
class TlsRecordReader : public common::NonCopyable, public common::NonMovable {
public:
    struct Result {
        enum class Status : std::uint8_t {
            NeedMore, // buffered bytes do not complete the next record yet
            Ok, // `record` engaged; owns its bytes, see next()
            Fatal, // framing violation; `alert` is the alert to emit
        };
        Status status = Status::NeedMore;
        TlsRecord record{};
        TlsAlertDesc alert = TlsAlertDesc::CloseNotify;
    };

    TlsRecordReader() noexcept = default;
    explicit TlsRecordReader(mem::IoBufNodePool &node_pool) noexcept : pending_(node_pool) {}

    // Only for the default constructor; must precede the first feed.
    void bind_node_pool(mem::IoBufNodePool &node_pool) noexcept { pending_.bind_node_pool(node_pool); }

    // Appends inbound bytes. Returns false only on allocation failure (feed a
    // chain bound to a different node pool). Feeding never disturbs records
    // already taken.
    bool feed(mem::IoBuf &&buf) noexcept { return pending_.append(std::move(buf)); }
    bool feed(mem::IoBufChain &&chain) noexcept { return pending_.append_chain(std::move(chain)); }

    // Splits the next record off the buffered input by taking its bytes out of
    // the pending chain. An Ok record owns its payload and stays valid for as
    // long as the caller keeps it — but its chain nodes belong to the reader's
    // IoBufNodePool, so destroy it on the loop that owns that pool.
    [[nodiscard]] Result next() noexcept;

    [[nodiscard]] std::size_t pending_bytes() const noexcept { return pending_.readable_bytes(); }

    void reset() noexcept;

private:
    mem::IoBufChain pending_;
};

} // namespace fiber::tls

#endif // FIBER_TLS_RECORD_TLS_RECORD_READER_H
