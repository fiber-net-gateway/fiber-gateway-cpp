#ifndef FIBER_HTTP_HTTP_CONNECTION_POOL_BUCKET_BASE_H
#define FIBER_HTTP_HTTP_CONNECTION_POOL_BUCKET_BASE_H

#include <cstdint>

namespace fiber::http {

class HttpConnectionPoolBucketBase {
public:
    static constexpr std::uint32_t kInvalidSlotIndex = 0xffffffffU;
    [[nodiscard]] std::uint32_t slot_index() const noexcept { return slot_index_; }

protected:
    friend class HttpConnectionBucketIndex;
    friend class Http1ConnectionPoolCore;
    std::uint32_t slot_index_ = kInvalidSlotIndex;
};

} // namespace fiber::http
#endif // FIBER_HTTP_HTTP_CONNECTION_POOL_BUCKET_BASE_H
