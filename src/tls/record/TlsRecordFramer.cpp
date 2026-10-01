#include <fiber/tls/record/TlsRecordFramer.h>

#include <limits>

#include <fiber/common/Assert.h>

namespace fiber::tls {

TlsFrameResult tls_frame_records(std::span<const std::uint8_t> wire, std::span<TlsRecordSpan> out) noexcept {
    FIBER_ASSERT(wire.size() <= std::numeric_limits<std::uint32_t>::max());
    TlsFrameResult result{};
    std::size_t off = 0;
    while (result.count < out.size() && wire.size() - off >= kTlsRecordHeaderSize) {
        const auto header = tls_decode_record_header(wire.data() + off);
        if (!header.has_value()) {
            result.fatal = true;
            result.alert = TlsAlertDesc::UnexpectedMessage;
            break;
        }
        if (header->length > kTlsMaxCiphertextRecordSize) {
            result.fatal = true;
            result.alert = TlsAlertDesc::RecordOverflow;
            break;
        }
        const std::size_t record_size = kTlsRecordHeaderSize + header->length;
        if (wire.size() - off < record_size) {
            break; // incomplete body: stays in the caller's buffer
        }
        out[result.count++] = TlsRecordSpan{
                .offset = static_cast<std::uint32_t>(off + kTlsRecordHeaderSize),
                .length = header->length,
                .legacy_version = header->legacy_version,
                .type = header->type,
        };
        off += record_size;
    }
    result.consumed = off;
    return result;
}

} // namespace fiber::tls
