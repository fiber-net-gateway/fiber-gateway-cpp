#ifndef FIBER_HTTP_HTTP2_CONNECTION_POOL_ENTRY_H
#define FIBER_HTTP_HTTP2_CONNECTION_POOL_ENTRY_H

#include <fiber/common/IntrusiveList.h>
#include <fiber/http/Http2ClientConnection.h>
#include <fiber/http/HttpConnectionPoolBucketBase.h>

namespace fiber::http {

class Http2ConnectionPoolCore;
class Http2ConnectionPoolGroupBucket;
class Http2PoolAcquireWaiter;

// Stable storage shared by all leases on one connection. Three independent
// hooks represent availability, ownership and idle expiry respectively.
//
// The pool core owns every field below; nothing outside it may touch them. They
// stay public only because IntrusiveList reaches the hooks through offsetof,
// which needs a standard-layout type, and that forbids mixing access levels.
class Http2ConnectionPoolEntry {
public:
    enum class State : std::uint8_t { Free, Connecting, Ready, Draining, Closed };
    ~Http2ConnectionPoolEntry();
    [[nodiscard]] Http2ClientConnection &connection() noexcept;

private:
    friend class Http2ConnectionPoolCore;

    void construct_connection(event::EventLoop &loop, Http2Connection::Options options) noexcept;
    void destroy_connection() noexcept;

public:
    common::IntrusiveListHook ready_hook_{};
    common::IntrusiveListHook group_hook_{};
    common::IntrusiveListHook idle_hook_{};
    Http2ConnectionPoolCore *pool_ = nullptr;
    Http2ConnectionPoolGroupBucket *bucket_ = nullptr;
    std::size_t active_leases_ = 0;
    std::size_t capacity_cache_ = 0;
    std::uint64_t served_streams_ = 0;
    std::chrono::steady_clock::time_point idle_since_{};
    State state_ = State::Free;
    bool dialing_ = false;
    bool maintenance_posted_ = false;
    bool has_connection_ = false;
    bool abort_connection_ = false;
    // Ready, but with no capacity purely because the peer's first SETTINGS has
    // not landed yet. Such a group waits instead of dialing another connection.
    bool awaiting_settings_ = false;
    event::EventLoop::DeferEntry maintenance_entry_{};
    Http2CloseGate::ObserverHook closed_observer_{};
    Http2ConnectionPoolEntry *next_free_ = nullptr;
    alignas(Http2ClientConnection) std::byte conn_storage_[sizeof(Http2ClientConnection)];
};

using Http2PoolReadyList =
        common::IntrusiveList<Http2ConnectionPoolEntry, offsetof(Http2ConnectionPoolEntry, ready_hook_)>;
using Http2PoolAllList =
        common::IntrusiveList<Http2ConnectionPoolEntry, offsetof(Http2ConnectionPoolEntry, group_hook_)>;
using Http2PoolIdleList =
        common::IntrusiveList<Http2ConnectionPoolEntry, offsetof(Http2ConnectionPoolEntry, idle_hook_)>;

class Http2ConnectionPoolGroupBucket : public HttpConnectionPoolBucketBase {
private:
    friend class Http2ConnectionPoolCore;
    friend class Http2PoolAcquireWaiter;
    Http2PoolReadyList ready_{};
    Http2PoolAllList all_{};
    std::size_t total_count_ = 0;
    std::size_t ready_count_ = 0;
    std::size_t connecting_count_ = 0;
    std::size_t awaiting_settings_count_ = 0;
    // Consecutive failed dials, driving the retry backoff. Reset by a success.
    std::size_t dial_failures_ = 0;
    Http2PoolAcquireWaiter *wait_head_ = nullptr;
    Http2PoolAcquireWaiter *wait_tail_ = nullptr;
    // A failed dial retries after a backoff timer, never a busy retry loop.
    event::EventLoop::TimerEntry retry_timer_{};
    Http2ConnectionPoolCore *pool_ = nullptr;
    Http2ConnectionPoolGroupBucket *next_free_ = nullptr;
};

} // namespace fiber::http
#endif // FIBER_HTTP_HTTP2_CONNECTION_POOL_ENTRY_H
