#include "ProtoCodec.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <sys/uio.h>
#include <vector>

#include <google/protobuf/io/zero_copy_stream.h>

namespace fiber::grpc {
namespace {

// Stack array for the common single/few-segment case. Chains with more readable
// segments than this fall back to a heap array (see IoBufChainInputStream below).
constexpr int kMaxStackIov = 16;

// ZeroCopyInputStream adapter over a read-only IoBufChain: yields each readable
// segment as a block so protobuf parses directly off the chain's storage, with no
// coalescing into a contiguous buffer. Only iovec pointers are cached at
// construction (no byte copies); the chain must outlive the adapter. The chain is
// not mutated during parsing, so the cached pointers stay valid.
class IoBufChainInputStream final : public google::protobuf::io::ZeroCopyInputStream {
public:
    explicit IoBufChainInputStream(const mem::IoBufChain &chain) noexcept {
        const int n = chain.fill_write_iov(stack_.data(), static_cast<int>(stack_.size()));
        std::size_t captured = 0;
        for (int i = 0; i < n; ++i) {
            captured += stack_[i].iov_len;
        }
        if (captured == chain.readable_bytes()) {
            iov_ = stack_.data();
            count_ = n;
        } else {
            // More readable segments than the stack array held: pull them all into a
            // heap array sized to the node count (an upper bound on readable segments).
            wide_.resize(chain.size());
            count_ = chain.fill_write_iov(wide_.data(), static_cast<int>(wide_.size()));
            iov_ = wide_.data();
        }
    }

    bool Next(const void **data, int *size) noexcept override {
        if (idx_ >= count_) {
            return false; // EOF: protobuf treats this as end-of-stream
        }
        *data = static_cast<const std::uint8_t *>(iov_[idx_].iov_base) + off_;
        const std::size_t remaining = iov_[idx_].iov_len - off_;
        *size = static_cast<int>(remaining);
        byte_count_ += static_cast<std::int64_t>(remaining);
        ++idx_;
        off_ = 0;
        return true;
    }

    void BackUp(int count) noexcept override {
        // protobuf only backs up within the last Next() block.
        --idx_;
        off_ = iov_[idx_].iov_len - static_cast<std::size_t>(count);
        byte_count_ -= count;
    }

    bool Skip(int count) noexcept override {
        byte_count_ += count;
        std::size_t remaining = static_cast<std::size_t>(count);
        while (remaining > 0) {
            if (idx_ >= count_) {
                return false;
            }
            const std::size_t avail = iov_[idx_].iov_len - off_;
            if (remaining < avail) {
                off_ += remaining;
                return true;
            }
            remaining -= avail;
            ++idx_;
            off_ = 0;
        }
        return true;
    }

    std::int64_t ByteCount() const noexcept override { return byte_count_; }

private:
    std::array<iovec, kMaxStackIov> stack_{};
    std::vector<iovec> wide_;
    const iovec *iov_ = nullptr;
    int count_ = 0;
    int idx_ = 0;
    std::size_t off_ = 0;
    std::int64_t byte_count_ = 0;
};

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

    // Multi-segment: parse directly off each readable segment via a
    // ZeroCopyInputStream adapter (no coalescing into a contiguous buffer).
    IoBufChainInputStream stream(chain);
    if (!out.ParseFromBoundedZeroCopyStream(&stream, static_cast<int>(n))) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return {};
}

} // namespace fiber::grpc
