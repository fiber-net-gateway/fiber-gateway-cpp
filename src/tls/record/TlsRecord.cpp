#include <fiber/tls/record/TlsRecord.h>

namespace fiber::tls {

bool tls_is_valid_content_type(std::uint8_t type) noexcept {
    switch (static_cast<TlsContentType>(type)) {
        case TlsContentType::ChangeCipherSpec:
        case TlsContentType::Alert:
        case TlsContentType::Handshake:
        case TlsContentType::ApplicationData:
            return true;
        default:
            return false;
    }
}

std::optional<TlsRecordHeader> tls_decode_record_header(const std::uint8_t *src) noexcept {
    if (!tls_is_valid_content_type(src[0])) {
        return std::nullopt;
    }
    TlsRecordHeader header{};
    header.type = static_cast<TlsContentType>(src[0]);
    header.legacy_version = static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[1]) << 8) | src[2]);
    header.length = static_cast<std::uint16_t>((static_cast<std::uint16_t>(src[3]) << 8) | src[4]);
    return header;
}

} // namespace fiber::tls
