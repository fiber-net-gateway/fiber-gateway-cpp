#ifndef FIBER_TLS_DETAIL_TLS_CURSOR_H
#define FIBER_TLS_DETAIL_TLS_CURSOR_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

#include "../../common/IoError.h"

namespace fiber::tls {

// Bounds-checked big-endian reader over one contiguous buffer, shared by the
// handshake/extension codecs. All reads fail with IoErr::Invalid past the end;
// no allocations, no exceptions.
class TlsReadCursor {
public:
    TlsReadCursor(const std::uint8_t *data, std::size_t len) noexcept : begin_(data), pos_(data), end_(data + len) {}

    [[nodiscard]] const std::uint8_t *begin() const noexcept { return begin_; }
    [[nodiscard]] const std::uint8_t *pos() const noexcept { return pos_; }
    [[nodiscard]] const std::uint8_t *end() const noexcept { return end_; }
    [[nodiscard]] std::size_t offset() const noexcept { return static_cast<std::size_t>(pos_ - begin_); }
    [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - pos_); }
    [[nodiscard]] bool empty() const noexcept { return pos_ == end_; }

    [[nodiscard]] common::IoResult<std::uint8_t> read_u8() noexcept {
        if (remaining() < 1) {
            return std::unexpected(common::IoErr::Invalid);
        }
        return *pos_++;
    }

    [[nodiscard]] common::IoResult<std::uint16_t> read_be16() noexcept {
        if (remaining() < 2) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const std::uint16_t value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(pos_[0]) << 8U) | pos_[1]);
        pos_ += 2;
        return value;
    }

    [[nodiscard]] common::IoResult<std::uint32_t> read_be24() noexcept {
        if (remaining() < 3) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const std::uint32_t value =
                (static_cast<std::uint32_t>(pos_[0]) << 16U) | (static_cast<std::uint32_t>(pos_[1]) << 8U) | pos_[2];
        pos_ += 3;
        return value;
    }

    [[nodiscard]] common::IoResult<std::uint32_t> read_be32() noexcept {
        if (remaining() < 4) {
            return std::unexpected(common::IoErr::Invalid);
        }
        const std::uint32_t value = (static_cast<std::uint32_t>(pos_[0]) << 24U) |
                                    (static_cast<std::uint32_t>(pos_[1]) << 16U) |
                                    (static_cast<std::uint32_t>(pos_[2]) << 8U) | pos_[3];
        pos_ += 4;
        return value;
    }

    [[nodiscard]] common::IoResult<std::span<const std::uint8_t>> read_slice(std::size_t len) noexcept {
        if (remaining() < len) {
            return std::unexpected(common::IoErr::Invalid);
        }
        std::span<const std::uint8_t> slice{pos_, len};
        pos_ += len;
        return slice;
    }

    [[nodiscard]] common::IoResult<void> skip(std::size_t len) noexcept {
        if (remaining() < len) {
            return std::unexpected(common::IoErr::Invalid);
        }
        pos_ += len;
        return {};
    }

private:
    const std::uint8_t *begin_ = nullptr;
    const std::uint8_t *pos_ = nullptr;
    const std::uint8_t *end_ = nullptr;
};

} // namespace fiber::tls

#endif // FIBER_TLS_DETAIL_TLS_CURSOR_H
