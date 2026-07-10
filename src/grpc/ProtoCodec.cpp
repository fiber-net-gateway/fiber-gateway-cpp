#include "ProtoCodec.h"

#include <array>
#include <string>
#include <sys/uio.h>
#include <vector>

namespace fiber::grpc {
namespace {

// Stack array for the common single/few-segment case. Chains with more readable
// segments than this fall back to a heap array (see decode() below).
constexpr int kMaxStackIov = 16;

} // namespace

common::IoResult<mem::IoBufChain> encode(mem::IoBufNodePool &node_pool,
                                         const google::protobuf::MessageLite &msg) noexcept {
    mem::IoBufChain chain(node_pool);
    const std::size_t n = msg.ByteSizeLong();
    if (n == 0) {
        return chain; // empty payload
    }

    mem::IoBuf buf = mem::IoBuf::allocate(n);
    if (!buf) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (!msg.SerializeToArray(buf.writable_data(), static_cast<int>(n))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    buf.commit(n);
    if (!chain.append(std::move(buf))) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return chain;
}

common::IoResult<void> decode(std::string_view bytes, google::protobuf::MessageLite &out) noexcept {
    if (!out.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

common::IoResult<void> decode(const mem::IoBufChain &chain, google::protobuf::MessageLite &out) noexcept {
    const std::size_t n = chain.readable_bytes();
    if (n == 0) {
        if (!out.ParseFromArray(nullptr, 0)) {
            return std::unexpected(common::IoErr::Invalid);
        }
        return {};
    }

    // Fast path: the whole payload is one contiguous readable node (zero-copy).
    if (const mem::IoBuf *head = chain.first_readable(); head != nullptr && head->readable() == n) {
        if (!out.ParseFromArray(head->readable_data(), static_cast<int>(n))) {
            return std::unexpected(common::IoErr::Invalid);
        }
        return {};
    }

    // Multi-segment: coalesce readable bytes into a contiguous buffer.
    std::string buf;
    buf.reserve(n);

    std::array<iovec, kMaxStackIov> iov{};
    int count = chain.fill_write_iov(iov.data(), static_cast<int>(iov.size()));
    std::size_t captured = 0;
    for (int i = 0; i < count; ++i) {
        buf.append(static_cast<const char *>(iov[i].iov_base), iov[i].iov_len);
        captured += iov[i].iov_len;
    }

    // If the stack array could not hold every readable segment, refill with a
    // heap array sized to the node count (an upper bound on readable segments).
    if (captured != n) {
        std::vector<iovec> wide(chain.size());
        int wide_count = chain.fill_write_iov(wide.data(), static_cast<int>(wide.size()));
        buf.clear();
        for (int i = 0; i < wide_count; ++i) {
            buf.append(static_cast<const char *>(wide[i].iov_base), wide[i].iov_len);
        }
    }

    if (!out.ParseFromArray(buf.data(), static_cast<int>(buf.size()))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

} // namespace fiber::grpc
