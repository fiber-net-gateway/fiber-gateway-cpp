#include <fiber/tls/record/TlsRecordReader.h>

#include <algorithm>
#include <cstring>

#include <fiber/common/Assert.h>

namespace fiber::tls {

void TlsRecordReader::reset() noexcept { pending_.clear(); }

TlsRecordReader::Result TlsRecordReader::next() noexcept {
    Result result{};
    if (pending_.readable_bytes() < kTlsRecordHeaderSize) {
        return result;
    }

    // Peek the header without consuming. Five bytes straddle at most five
    // readable spans, so five iov slots always cover them.
    struct iovec spans[kTlsRecordHeaderSize];
    const int span_count = pending_.fill_write_iov(spans, static_cast<int>(kTlsRecordHeaderSize));
    FIBER_ASSERT(span_count > 0);

    std::uint8_t header[kTlsRecordHeaderSize]{};
    std::size_t filled = 0;
    for (int i = 0; i < span_count && filled < kTlsRecordHeaderSize; ++i) {
        const std::size_t take = std::min<std::size_t>(spans[i].iov_len, kTlsRecordHeaderSize - filled);
        std::memcpy(header + filled, spans[i].iov_base, take);
        filled += take;
    }
    FIBER_ASSERT(filled == kTlsRecordHeaderSize);

    const auto decoded = tls_decode_record_header(header);
    if (!decoded.has_value()) {
        result.status = Result::Status::Fatal;
        result.alert = TlsAlertDesc::UnexpectedMessage;
        return result;
    }
    if (decoded->length > kTlsMaxCiphertextRecordSize) {
        result.status = Result::Status::Fatal;
        result.alert = TlsAlertDesc::RecordOverflow;
        return result;
    }

    const std::size_t record_size = kTlsRecordHeaderSize + decoded->length;
    if (pending_.readable_bytes() < record_size) {
        return result;
    }

    result.status = Result::Status::Ok;
    result.record.type = decoded->type;
    result.record.legacy_version = decoded->legacy_version;
    result.record.length = decoded->length;

    // Zero-copy split: take the record's bytes out of the pending chain —
    // whole nodes move, a node straddled by the record boundary is split via a
    // retained view. take_prefix rolls back cleanly on allocation failure.
    // The header stays as the record chain's consumed prefix.
    if (!pending_.take_prefix(record_size, result.record.payload)) {
        result.record = TlsRecord{};
        result.status = Result::Status::Fatal;
        result.alert = TlsAlertDesc::InternalError;
        return result;
    }
    result.record.payload.consume(kTlsRecordHeaderSize);
    return result;
}

} // namespace fiber::tls
