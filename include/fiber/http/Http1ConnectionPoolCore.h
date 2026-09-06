#ifndef FIBER_HTTP_HTTP1_CONNECTION_POOL_CORE_H
#define FIBER_HTTP_HTTP1_CONNECTION_POOL_CORE_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "Http1ConnectionPoolEntry.h"
#include "HttpConnectionBucketIndex.h"
#include "HttpConnectionGroupKey.h"

namespace fiber::http {

class Http1ConnectionPoolCore : public common::NonCopyable, public common::NonMovable {
public:
    using IdleCountChangedCallback = void (*)(void *ctx, const HttpConnectionGroupKey &key,
                                              std::size_t idle_count) noexcept;

    class Lease : public common::NonCopyable {
    public:
        Lease() noexcept = default;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease();

        [[nodiscard]] bool valid() const noexcept { return pool_ != nullptr && key_.has_value(); }
        [[nodiscard]] bool has_connection() const noexcept { return entry_ != nullptr && entry_->has_connection(); }
        [[nodiscard]] bool hit() const noexcept { return hit_; }

        [[nodiscard]] Http1ClientConnection *get() noexcept { return entry_ ? entry_->connection() : nullptr; }
        [[nodiscard]] const Http1ClientConnection *get() const noexcept {
            return entry_ ? entry_->connection() : nullptr;
        }

        [[nodiscard]] Http1ClientConnection &connection() noexcept;
        [[nodiscard]] const HttpConnectionGroupKey &key() const noexcept;
        // Creates the unconnected connection this lease's slot will hold. The caller dials it
        // through one of Http1ClientConnection::connect's overloads; the lease's key already
        // fixes the transport profile, so nothing about it is passed in here.
        [[nodiscard]] common::IoResult<Http1ClientConnection *> emplace_connection() noexcept;
        void reset() noexcept;

    private:
        friend class Http1ConnectionPoolCore;

        Lease(Http1ConnectionPoolCore &pool, Http1ConnectionPoolEntry *entry, const HttpConnectionGroupKey &key,
              bool hit) noexcept;

        Http1ConnectionPoolCore *pool_ = nullptr;
        Http1ConnectionPoolEntry *entry_ = nullptr;
        std::optional<HttpConnectionGroupKey> key_{};
        bool hit_ = false;
    };

    struct Options {
        std::size_t max_idle_per_group = 2;
        std::size_t max_idle_total = 64;
        std::chrono::milliseconds idle_timeout{30000};
        std::size_t initial_group_capacity = 0;
    };

    explicit Http1ConnectionPoolCore(event::EventLoop &loop) noexcept;
    Http1ConnectionPoolCore(event::EventLoop &loop, Options options) noexcept;
    ~Http1ConnectionPoolCore();

    [[nodiscard]] bool init() noexcept;
    [[nodiscard]] Lease acquire(const HttpConnectionGroupKey &key) noexcept;
    [[nodiscard]] Http1ConnectionPoolEntry *try_steal_idle_entry(const HttpConnectionGroupKey &key) noexcept;
    void accept_returned_entry(Http1ConnectionPoolEntry &entry, const HttpConnectionGroupKey &key) noexcept;
    void shutdown() noexcept;
    void clear() noexcept;
    void set_idle_count_changed_callback(IdleCountChangedCallback cb, void *ctx) noexcept {
        idle_count_changed_cb_ = cb;
        idle_count_changed_ctx_ = ctx;
    }
    void clear_idle_count_changed_callback() noexcept {
        idle_count_changed_cb_ = nullptr;
        idle_count_changed_ctx_ = nullptr;
    }
    void set_external_shutdown_flag(const std::atomic<bool> *flag) noexcept { external_shutdown_flag_ = flag; }
    void clear_external_shutdown_flag() noexcept { external_shutdown_flag_ = nullptr; }

    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] const Options &options() const noexcept { return options_; }
    [[nodiscard]] std::size_t idle_total() const noexcept { return idle_total_; }
    [[nodiscard]] std::size_t group_count() const noexcept { return bucket_index_.size(); }
    [[nodiscard]] bool shutdown_requested() const noexcept;

private:
    friend class Lease;

    static Options normalize_options(Options options) noexcept;
    static void on_expiry_timer(Http1ConnectionPoolCore *pool) noexcept;
    [[nodiscard]] bool shutdown_effective() const noexcept;

    [[nodiscard]] bool entry_expired(const Http1ConnectionPoolEntry &entry,
                                     std::chrono::steady_clock::time_point now) const noexcept;
    [[nodiscard]] Http1ConnectionPoolGroupBucket *allocate_bucket() noexcept;
    [[nodiscard]] Http1ConnectionPoolEntry *allocate_entry() noexcept;
    void recycle_bucket(Http1ConnectionPoolGroupBucket *bucket) noexcept;
    void recycle_entry(Http1ConnectionPoolEntry *entry) noexcept;
    void destroy_free_lists() noexcept;

    void arm_expiry_timer(std::chrono::steady_clock::time_point when) noexcept;
    void arm_expiry_timer_if_needed() noexcept;
    void cancel_expiry_timer() noexcept;
    void on_expiry_timer_fired() noexcept;
    void evict_expired_entries(std::chrono::steady_clock::time_point now) noexcept;
    void park_entry(Http1ConnectionPoolEntry &entry, const HttpConnectionGroupKey &key) noexcept;
    void release_lease(Lease &lease) noexcept;
    void detach_idle_entry(Http1ConnectionPoolEntry &entry) noexcept;
    void evict_entry(Http1ConnectionPoolEntry &entry) noexcept;
    void evict_group_oldest(Http1ConnectionPoolGroupBucket &bucket) noexcept;
    void evict_global_oldest() noexcept;

    event::EventLoop *loop_ = nullptr;
    Options options_{};
    HttpConnectionBucketIndex bucket_index_{};
    Http1ConnectionPoolGlobalList global_idle_entries_{};
    event::EventLoop::TimerEntry expiry_timer_{};
    Http1ConnectionPoolGroupBucket *free_bucket_head_ = nullptr;
    Http1ConnectionPoolEntry *free_entry_head_ = nullptr;
    std::size_t idle_total_ = 0;
    IdleCountChangedCallback idle_count_changed_cb_ = nullptr;
    void *idle_count_changed_ctx_ = nullptr;
    const std::atomic<bool> *external_shutdown_flag_ = nullptr;
    bool shutdown_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_POOL_CORE_H
