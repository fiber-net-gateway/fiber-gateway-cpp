#ifndef FIBER_QUIC_QUIC_CURSOR_H
#define FIBER_QUIC_QUIC_CURSOR_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>

#include "../common/IoError.h"
#include "QuicProtocol.h"

namespace fiber::quic {

class QuicReadCursor {
public:
    QuicReadCursor(const std::uint8_t *data, std::size_t len) noexcept : begin_(data), pos_(data), end_(data + len) {}

    [[nodiscard]] const std::uint8_t *begin() const noexcept { return begin_; }
    [[nodiscard]] const std::uint8_t *pos() const noexcept { return pos_; }
    [[nodiscard]] const std::uint8_t *end() const noexcept { return end_; }
    [[nodiscard]] std::size_t offset() const noexcept { return static_cast<std::size_t>(pos_ - begin_); }
    [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - pos_); }
    [[nodiscard]] bool empty() const noexcept { return pos_ == end_; }

    common::IoResult<std::uint8_t> read_u8() noexcept;
    common::IoResult<std::uint16_t> read_be16() noexcept;
    common::IoResult<std::uint32_t> read_be24() noexcept;
    common::IoResult<std::uint32_t> read_be32() noexcept;
    common::IoResult<QuicSlice> read_slice(std::size_t len) noexcept;
    common::IoResult<void> copy_bytes(std::uint8_t *dst, std::size_t len) noexcept;
    common::IoResult<void> skip(std::size_t len) noexcept;

private:
    const std::uint8_t *begin_ = nullptr;
    const std::uint8_t *pos_ = nullptr;
    const std::uint8_t *end_ = nullptr;
};

class QuicWriteCursor {
public:
    QuicWriteCursor(std::uint8_t *data, std::size_t cap) noexcept : begin_(data), pos_(data), end_(data + cap) {}

    [[nodiscard]] std::uint8_t *begin() noexcept { return begin_; }
    [[nodiscard]] std::uint8_t *pos() noexcept { return pos_; }
    [[nodiscard]] const std::uint8_t *pos() const noexcept { return pos_; }
    [[nodiscard]] std::size_t offset() const noexcept { return static_cast<std::size_t>(pos_ - begin_); }
    [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - pos_); }

    common::IoResult<void> write_u8(std::uint8_t value) noexcept;
    common::IoResult<void> write_be16(std::uint16_t value) noexcept;
    common::IoResult<void> write_be24(std::uint32_t value) noexcept;
    common::IoResult<void> write_be32(std::uint32_t value) noexcept;
    common::IoResult<void> write_bytes(const std::uint8_t *data, std::size_t len) noexcept;
    common::IoResult<void> fill(std::uint8_t value, std::size_t len) noexcept;

private:
    std::uint8_t *begin_ = nullptr;
    std::uint8_t *pos_ = nullptr;
    std::uint8_t *end_ = nullptr;
};

inline common::IoResult<std::uint8_t> QuicReadCursor::read_u8() noexcept {
    if (remaining() < 1) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return *pos_++;
}

inline common::IoResult<std::uint16_t> QuicReadCursor::read_be16() noexcept {
    if (remaining() < 2) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint16_t value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(pos_[0]) << 8U) | pos_[1]);
    pos_ += 2;
    return value;
}

inline common::IoResult<std::uint32_t> QuicReadCursor::read_be24() noexcept {
    if (remaining() < 3) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint32_t value =
            (static_cast<std::uint32_t>(pos_[0]) << 16U) | (static_cast<std::uint32_t>(pos_[1]) << 8U) | pos_[2];
    pos_ += 3;
    return value;
}

inline common::IoResult<std::uint32_t> QuicReadCursor::read_be32() noexcept {
    if (remaining() < 4) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::uint32_t value = (static_cast<std::uint32_t>(pos_[0]) << 24U) |
                                (static_cast<std::uint32_t>(pos_[1]) << 16U) |
                                (static_cast<std::uint32_t>(pos_[2]) << 8U) | pos_[3];
    pos_ += 4;
    return value;
}

inline common::IoResult<QuicSlice> QuicReadCursor::read_slice(std::size_t len) noexcept {
    if (remaining() < len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    QuicSlice slice{pos_, len};
    pos_ += len;
    return slice;
}

inline common::IoResult<void> QuicReadCursor::copy_bytes(std::uint8_t *dst, std::size_t len) noexcept {
    if ((dst == nullptr && len != 0) || remaining() < len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (len != 0) {
        std::memcpy(dst, pos_, len);
    }
    pos_ += len;
    return {};
}

inline common::IoResult<void> QuicReadCursor::skip(std::size_t len) noexcept {
    if (remaining() < len) {
        return std::unexpected(common::IoErr::Invalid);
    }
    pos_ += len;
    return {};
}

inline common::IoResult<void> QuicWriteCursor::write_u8(std::uint8_t value) noexcept {
    if (remaining() < 1) {
        return std::unexpected(common::IoErr::NoMem);
    }
    *pos_++ = value;
    return {};
}

inline common::IoResult<void> QuicWriteCursor::write_be16(std::uint16_t value) noexcept {
    if (remaining() < 2) {
        return std::unexpected(common::IoErr::NoMem);
    }
    pos_[0] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    pos_[1] = static_cast<std::uint8_t>(value & 0xffU);
    pos_ += 2;
    return {};
}

inline common::IoResult<void> QuicWriteCursor::write_be24(std::uint32_t value) noexcept {
    if (remaining() < 3) {
        return std::unexpected(common::IoErr::NoMem);
    }
    pos_[0] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    pos_[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    pos_[2] = static_cast<std::uint8_t>(value & 0xffU);
    pos_ += 3;
    return {};
}

inline common::IoResult<void> QuicWriteCursor::write_be32(std::uint32_t value) noexcept {
    if (remaining() < 4) {
        return std::unexpected(common::IoErr::NoMem);
    }
    pos_[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    pos_[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    pos_[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    pos_[3] = static_cast<std::uint8_t>(value & 0xffU);
    pos_ += 4;
    return {};
}

inline common::IoResult<void> QuicWriteCursor::write_bytes(const std::uint8_t *data, std::size_t len) noexcept {
    if ((data == nullptr && len != 0) || remaining() < len) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (len != 0) {
        std::memcpy(pos_, data, len);
    }
    pos_ += len;
    return {};
}

inline common::IoResult<void> QuicWriteCursor::fill(std::uint8_t value, std::size_t len) noexcept {
    if (remaining() < len) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (len != 0) {
        std::memset(pos_, value, len);
    }
    pos_ += len;
    return {};
}

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_CURSOR_H
