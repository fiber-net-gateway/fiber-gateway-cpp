#include "GrpcFraming.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sys/uio.h>

namespace fiber::grpc {
namespace {

constexpr std::size_t kFrameHeaderSize = 5;

} // namespace

common::IoResult<mem::IoBufChain> frame(mem::IoBufChain payload) noexcept {
    if (!payload.bound()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    const std::size_t n = payload.readable_bytes();
    if (n > 0xffffffffull) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    mem::IoBuf header = mem::IoBuf::allocate(kFrameHeaderSize);
    if (!header) {
        return std::unexpected(common::IoErr::NoMem);
    }
    std::uint8_t *p = header.writable_data();
    p[0] = 0; // no compression
    const std::uint32_t be = static_cast<std::uint32_t>(n);
    p[1] = static_cast<std::uint8_t>(be >> 24);
    p[2] = static_cast<std::uint8_t>(be >> 16);
    p[3] = static_cast<std::uint8_t>(be >> 8);
    p[4] = static_cast<std::uint8_t>(be);
    header.commit(kFrameHeaderSize);

    if (!payload.prepend(std::move(header))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return payload;
}

common::IoResult<void> GrpcFrameReader::append(mem::IoBufChain chunk) noexcept {
    if (!buffer_.bound()) {
        // First chunk: adopt its node-pool binding and nodes wholesale (zero-copy).
        buffer_ = std::move(chunk);
        return {};
    }
    // Subsequent chunks (must share the reader's pool): move each node across.
    while (auto *node = chunk.pop_front_node()) {
        if (!buffer_.append_node(node)) {
            buffer_.node_pool().release(node);
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    return {};
}

common::IoResult<bool> GrpcFrameReader::next_payload(mem::IoBufChain &out) noexcept {
    if (buffer_.readable_bytes() < kFrameHeaderSize) {
        return false; // need more bytes for the header
    }

    // Peek the 5-byte header without consuming. fill_write_iov yields readable
    // segments in order; since readable_bytes() >= 5 and every returned segment
    // has >=1 byte, 5 segments always cover at least 5 bytes.
    std::array<iovec, kFrameHeaderSize> iov{};
    const int c = buffer_.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::uint8_t hdr[kFrameHeaderSize]{};
    std::size_t off = 0;
    for (int i = 0; i < c && off < kFrameHeaderSize; ++i) {
        const std::size_t take = std::min(iov[i].iov_len, kFrameHeaderSize - off);
        std::memcpy(hdr + off, iov[i].iov_base, take);
        off += take;
    }

    if (hdr[0] != 0) {
        return std::unexpected(common::IoErr::NotSupported); // compressed frames not supported
    }
    const std::uint32_t len = (static_cast<std::uint32_t>(hdr[1]) << 24) | (static_cast<std::uint32_t>(hdr[2]) << 16) |
                              (static_cast<std::uint32_t>(hdr[3]) << 8) | static_cast<std::uint32_t>(hdr[4]);
    if (buffer_.readable_bytes() < kFrameHeaderSize + len) {
        return false; // need more bytes for the payload
    }

    buffer_.consume(kFrameHeaderSize); // drop the header
    buffer_.drop_empty_front(); // release emptied header nodes
    if (!buffer_.take_prefix(len, out)) { // zero-copy payload extract
        return std::unexpected(common::IoErr::NoMem);
    }
    return true;
}

std::size_t GrpcFrameReader::buffered_bytes() const noexcept { return buffer_.readable_bytes(); }

} // namespace fiber::grpc
