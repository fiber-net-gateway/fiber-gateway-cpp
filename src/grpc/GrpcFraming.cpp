#include "GrpcFraming.h"

#include <array>
#include <cstdint>
#include <sys/uio.h>
#include <vector>

namespace fiber::grpc {
namespace {

constexpr std::size_t kFrameHeaderSize = 5;

// Append all readable bytes of `chain` into `dst` (multi-segment aware).
void append_chain_bytes(std::string &dst, const mem::IoBufChain &chain) {
    const std::size_t total = chain.readable_bytes();
    if (total == 0) {
        return;
    }
    // Fast path: single contiguous readable node.
    if (const mem::IoBuf *head = chain.first_readable(); head != nullptr && head->readable() == total) {
        dst.append(reinterpret_cast<const char *>(head->readable_data()), total);
        return;
    }
    std::array<iovec, 16> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::size_t captured = 0;
    for (int i = 0; i < count; ++i) {
        dst.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
        captured += iov[i].iov_len;
    }
    // More readable segments than the stack array held: refill with a heap array
    // sized to the node count (an upper bound on readable segments).
    if (captured != total) {
        std::vector<iovec> wide(chain.size());
        int wide_count = chain.fill_write_iov(wide.data(), static_cast<int>(wide.size()));
        for (int i = 0; i < wide_count; ++i) {
            dst.append(static_cast<const char *>(wide[i].iov_base), wide[i].iov_len);
        }
    }
}

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

common::IoResult<void> GrpcFrameReader::append(std::string_view bytes) noexcept {
    buffer_.append(bytes.data(), bytes.size());
    return {};
}

common::IoResult<void> GrpcFrameReader::append(const mem::IoBufChain &chain) noexcept {
    append_chain_bytes(buffer_, chain);
    return {};
}

common::IoResult<bool> GrpcFrameReader::next_payload(std::string &out) noexcept {
    if (buffer_.size() < kFrameHeaderSize) {
        return false;
    }
    const auto flag = static_cast<unsigned char>(buffer_[0]);
    if (flag != 0) {
        return std::unexpected(common::IoErr::NotSupported); // compressed frames not supported
    }
    const std::uint32_t len = (static_cast<std::uint8_t>(buffer_[1]) << 24) |
                              (static_cast<std::uint8_t>(buffer_[2]) << 16) |
                              (static_cast<std::uint8_t>(buffer_[3]) << 8) | static_cast<std::uint8_t>(buffer_[4]);
    const std::size_t total = kFrameHeaderSize + len;
    if (buffer_.size() < total) {
        return false; // need more bytes
    }
    out.assign(buffer_.data() + kFrameHeaderSize, len);
    buffer_.erase(0, total);
    return true;
}

std::size_t GrpcFrameReader::buffered_bytes() const noexcept { return buffer_.size(); }

} // namespace fiber::grpc
