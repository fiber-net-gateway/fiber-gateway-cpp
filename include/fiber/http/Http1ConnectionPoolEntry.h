#ifndef FIBER_HTTP_HTTP1_CONNECTION_POOL_ENTRY_H
#define FIBER_HTTP_HTTP1_CONNECTION_POOL_ENTRY_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

#include "../common/Assert.h"
#include "../common/IntrusiveList.h"
#include "../event/EventLoop.h"
#include "Http1ClientConnection.h"
#include "HttpConnectionGroupKey.h"
#include "HttpConnectionPoolBucketBase.h"

namespace fiber::http {

class Http1ConnectionPoolCore;
class Http1ConnectionPoolGroupBucket;
class HttpConnectionBucketIndex;
class StealableHttp1ConnectionPoolSet;

class Http1ConnectionPoolEntry {
public:
    Http1ConnectionPoolEntry() = default;
    ~Http1ConnectionPoolEntry() { destroy_connection(); }

    [[nodiscard]] bool has_connection() const noexcept { return has_connection_; }
    [[nodiscard]] Http1ClientConnection *connection() noexcept {
        return has_connection_ ? std::launder(reinterpret_cast<Http1ClientConnection *>(conn_storage_)) : nullptr;
    }
    [[nodiscard]] const Http1ClientConnection *connection() const noexcept {
        return has_connection_ ? std::launder(reinterpret_cast<const Http1ClientConnection *>(conn_storage_)) : nullptr;
    }

private:
    friend class Http1ConnectionPoolCore;

    void construct_connection(event::EventLoop &loop) noexcept {
        FIBER_ASSERT(!has_connection_);
        std::construct_at(connection_storage(), loop);
        has_connection_ = true;
    }

    void destroy_connection() noexcept {
        if (!has_connection_) {
            return;
        }
        std::destroy_at(connection_storage());
        has_connection_ = false;
    }

    [[nodiscard]] Http1ClientConnection *connection_storage() noexcept {
        return std::launder(reinterpret_cast<Http1ClientConnection *>(conn_storage_));
    }

    [[nodiscard]] const Http1ClientConnection *connection_storage() const noexcept {
        return std::launder(reinterpret_cast<const Http1ClientConnection *>(conn_storage_));
    }

public:
    common::IntrusiveListHook group_hook_{};
    common::IntrusiveListHook global_hook_{};
    Http1ConnectionPoolGroupBucket *bucket_ = nullptr;
    std::chrono::steady_clock::time_point idle_since_{};
    Http1ConnectionPoolEntry *next_free_ = nullptr;
    bool has_connection_ = false;
    alignas(Http1ClientConnection) std::byte conn_storage_[sizeof(Http1ClientConnection)]{};
};

inline constexpr std::size_t kHttp1ConnectionPoolEntryGroupHookOffset = offsetof(Http1ConnectionPoolEntry, group_hook_);
inline constexpr std::size_t kHttp1ConnectionPoolEntryGlobalHookOffset =
        offsetof(Http1ConnectionPoolEntry, global_hook_);

using Http1ConnectionPoolGroupList =
        common::IntrusiveList<Http1ConnectionPoolEntry, kHttp1ConnectionPoolEntryGroupHookOffset>;
using Http1ConnectionPoolGlobalList =
        common::IntrusiveList<Http1ConnectionPoolEntry, kHttp1ConnectionPoolEntryGlobalHookOffset>;

class Http1ConnectionPoolGroupBucket : public HttpConnectionPoolBucketBase {
private:
    friend class HttpConnectionBucketIndex;
    friend class Http1ConnectionPoolCore;

    std::size_t idle_count_ = 0;
    Http1ConnectionPoolGroupList idle_entries_{};
    Http1ConnectionPoolGroupBucket *next_free_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_POOL_ENTRY_H
