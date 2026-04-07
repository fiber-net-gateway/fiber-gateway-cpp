#ifndef FIBER_HTTP_HTTP1_CONNECTION_POOL_CORE_H
#define FIBER_HTTP_HTTP1_CONNECTION_POOL_CORE_H

#include <chrono>
#include <cstddef>
#include <optional>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "../event/EventLoop.h"
#include "Http1ConnectionBucketIndex.h"
#include "Http1ConnectionGroupKey.h"
#include "Http1ConnectionPoolEntry.h"

namespace fiber::http {

class Http1ConnectionPoolCore : public common::NonCopyable, public common::NonMovable {
public:
    using IdleCountChangedCallback =
        void (*)(void *ctx, const Http1ConnectionGroupKey &key, std::size_t idle_count) noexcept;

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
        [[nodiscard]] const Http1ClientConnection *get() const noexcept { return entry_ ? entry_->connection() : nullptr; }

        [[nodiscard]] Http1ClientConnection &connection() noexcept;
        [[nodiscard]] const Http1ConnectionGroupKey &key() const noexcept;
        [[nodiscard]] common::IoResult<Http1ClientConnection *> emplace_connection(Http1ClientConnectionOptions options) noexcept;
        void reset() noexcept;

    private:
        friend class Http1ConnectionPoolCore;

        Lease(Http1ConnectionPoolCore &pool, Http1ConnectionPoolEntry *entry, const Http1ConnectionGroupKey &key,
              bool hit) noexcept;

        Http1ConnectionPoolCore *pool_ = nullptr;
        Http1ConnectionPoolEntry *entry_ = nullptr;
        std::optional<Http1ConnectionGroupKey> key_{};
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
    [[nodiscard]] Lease acquire(const Http1ConnectionGroupKey &key) noexcept;
    [[nodiscard]] Http1ConnectionPoolEntry *try_steal_idle_entry(const Http1ConnectionGroupKey &key) noexcept;
    void accept_returned_entry(Http1ConnectionPoolEntry &entry, const Http1ConnectionGroupKey &key) noexcept;
    void sweep_expired(std::chrono::steady_clock::time_point now) noexcept;
    void clear() noexcept;
    void set_idle_count_changed_callback(IdleCountChangedCallback cb, void *ctx) noexcept {
        idle_count_changed_cb_ = cb;
        idle_count_changed_ctx_ = ctx;
    }
    void clear_idle_count_changed_callback() noexcept {
        idle_count_changed_cb_ = nullptr;
        idle_count_changed_ctx_ = nullptr;
    }

    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] const Options &options() const noexcept { return options_; }
    [[nodiscard]] std::size_t idle_total() const noexcept { return idle_total_; }
    [[nodiscard]] std::size_t group_count() const noexcept { return bucket_index_.size(); }
private:
    friend class Lease;

    static Options normalize_options(Options options) noexcept;

    [[nodiscard]] bool entry_expired(const Http1ConnectionPoolEntry &entry,
                                     std::chrono::steady_clock::time_point now) const noexcept;
    [[nodiscard]] bool entry_reusable(const Http1ConnectionPoolEntry &entry) const noexcept;
    [[nodiscard]] Http1ConnectionPoolGroupBucket *allocate_bucket() noexcept;
    [[nodiscard]] Http1ConnectionPoolEntry *allocate_entry() noexcept;
    void recycle_bucket(Http1ConnectionPoolGroupBucket *bucket) noexcept;
    void recycle_entry(Http1ConnectionPoolEntry *entry) noexcept;
    void destroy_free_lists() noexcept;

    void park_entry(Http1ConnectionPoolEntry &entry, const Http1ConnectionGroupKey &key) noexcept;
    void release_lease(Lease &lease) noexcept;
    void detach_idle_entry(Http1ConnectionPoolEntry &entry) noexcept;
    void evict_entry(Http1ConnectionPoolEntry &entry) noexcept;
    void evict_group_oldest(Http1ConnectionPoolGroupBucket &bucket) noexcept;
    void evict_global_oldest() noexcept;

    event::EventLoop *loop_ = nullptr;
    Options options_{};
    Http1ConnectionBucketIndex bucket_index_{};
    Http1ConnectionPoolGlobalList global_idle_entries_{};
    Http1ConnectionPoolGroupBucket *free_bucket_head_ = nullptr;
    Http1ConnectionPoolEntry *free_entry_head_ = nullptr;
    std::size_t idle_total_ = 0;
    IdleCountChangedCallback idle_count_changed_cb_ = nullptr;
    void *idle_count_changed_ctx_ = nullptr;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_POOL_CORE_H
