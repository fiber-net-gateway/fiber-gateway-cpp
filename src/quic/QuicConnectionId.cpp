#include <fiber/quic/QuicConnectionId.h>

#include <cstring>

namespace fiber::quic {

common::IoResult<QuicConnectionId> QuicConnectionId::from_bytes(const std::uint8_t *data, std::size_t len) noexcept {
    if (len > kMaxConnectionIdLength || (len > 0 && data == nullptr)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    QuicConnectionId out{};
    out.length_ = static_cast<std::uint8_t>(len);
    if (len > 0) {
        std::memcpy(out.bytes_.data(), data, len);
    }
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= out.bytes_[i];
        hash *= kFnvPrime;
    }
    out.hash_ = (hash ^ len) * kFnvPrime;
    return out;
}

} // namespace fiber::quic
