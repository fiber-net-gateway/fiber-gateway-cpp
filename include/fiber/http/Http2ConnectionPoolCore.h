#ifndef FIBER_HTTP_HTTP2_CONNECTION_POOL_CORE_H
#define FIBER_HTTP_HTTP2_CONNECTION_POOL_CORE_H

#include <atomic>
#include <optional>

#include <fiber/http/ClientHttp2Exchange.h>
#include <fiber/http/Http2ConnectionPoolEntry.h>
#include <fiber/http/HttpConnectionBucketIndex.h>

namespace fiber::http {

class Http2ConnectionPoolCore : public common::NonCopyable, public common::NonMovable {
public:
    struct Options {
        std::size_t max_streams_per_connection = 0;
        std::uint32_t pre_settings_max_streams = 16;
        std::uint64_t max_streams_lifetime = 0;
        std::size_t max_connections_per_group = 4;
        std::size_t max_connections_total = 64;
        std::size_t max_concurrent_dials_per_group = 1;
        std::size_t max_idle_total = 16;
        std::chrono::milliseconds idle_timeout{60000};
        // Backoff a group waits after a failed dial before anyone retries it.
        // Doubles per consecutive failure up to max_dial_retry_backoff, and
        // resets on the first success.
        std::chrono::milliseconds dial_retry_backoff{10};
        std::chrono::milliseconds max_dial_retry_backoff{1000};
        std::size_t initial_group_capacity = 0;
        Http2Connection::Options h2{};
    };
    struct Connector {
        // Borrowed for acquire's duration. Must support coroutine destruction
        // while suspended (deadline, clear, shutdown or caller cancellation).
        async::Task<common::IoResult<void>> (*connect)(void *ctx, Http2ClientConnection &conn,
                                                       const HttpConnectionGroupKey &key) noexcept = nullptr;
        void *ctx = nullptr;
    };
    using ConnCountChangedCallback = void (*)(void *ctx, const HttpConnectionGroupKey &key, std::size_t total,
                                              std::size_t ready) noexcept;
    // Reports every failed dial, including those an unbounded acquire will
    // silently retry. retry_after is the backoff before the group dials again.
    using DialFailedCallback = void (*)(void *ctx, const HttpConnectionGroupKey &key, common::IoErr error,
                                        std::size_t consecutive_failures,
                                        std::chrono::milliseconds retry_after) noexcept;
    class Lease : public common::NonCopyable {
    public:
        Lease() noexcept = default;
        Lease(Lease &&other) noexcept;
        Lease &operator=(Lease &&other) noexcept;
        ~Lease();
        [[nodiscard]] bool valid() const noexcept { return entry_ != nullptr; }
        [[nodiscard]] Http2ClientConnection &connection() noexcept;
        // The reference is invalidated by index growth or another group erase.
        [[nodiscard]] const HttpConnectionGroupKey &key() const noexcept;
        [[nodiscard]] ClientHttp2Exchange open_exchange(mem::BufPool &pool) noexcept;
        void reset() noexcept;

    private:
        friend class Http2ConnectionPoolCore;
        explicit Lease(Http2ConnectionPoolEntry &entry) noexcept : entry_(&entry) {}
        Http2ConnectionPoolEntry *entry_ = nullptr;
    };

    explicit Http2ConnectionPoolCore(event::EventLoop &loop) noexcept;
    Http2ConnectionPoolCore(event::EventLoop &loop, Options options) noexcept;
    ~Http2ConnectionPoolCore();
    [[nodiscard]] bool init() noexcept;
    // Values live in the coroutine frame, including when passed temporaries.
    // Zero only polls existing capacity and never starts a dial.
    [[nodiscard]] async::Task<common::IoResult<Lease>>
    acquire(HttpConnectionGroupKey key, Connector connector,
            std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) noexcept;
    // Synchronous reuse-only fast path: takes a slot on an already usable
    // connection, or nothing. It never dials, waits, yields to queued waiters,
    // or creates a group, so it borrows the key and builds no coroutine frame.
    // Fall back to acquire() when it comes back empty.
    [[nodiscard]] std::optional<Lease> try_acquire(const HttpConnectionGroupKey &key) noexcept;
    void clear() noexcept;
    void shutdown() noexcept;
    // Wait for all entries, dials and acquires to leave; release leases first.
    [[nodiscard]] async::Task<void> join() noexcept;
    void set_external_shutdown_flag(const std::atomic<bool> *flag) noexcept { external_shutdown_flag_ = flag; }
    void clear_external_shutdown_flag() noexcept { external_shutdown_flag_ = nullptr; }
    // Synchronous, observation-only callback. The key is borrowed for the call.
    void set_conn_count_changed_callback(ConnCountChangedCallback cb, void *ctx) noexcept {
        count_cb_ = cb;
        count_ctx_ = ctx;
    }
    void clear_conn_count_changed_callback() noexcept {
        count_cb_ = nullptr;
        count_ctx_ = nullptr;
    }
    // Synchronous, observation-only callback. The key is borrowed for the call.
    void set_dial_failed_callback(DialFailedCallback cb, void *ctx) noexcept {
        dial_failed_cb_ = cb;
        dial_failed_ctx_ = ctx;
    }
    void clear_dial_failed_callback() noexcept {
        dial_failed_cb_ = nullptr;
        dial_failed_ctx_ = nullptr;
    }
    [[nodiscard]] event::EventLoop &loop() const noexcept { return *loop_; }
    [[nodiscard]] const Options &options() const noexcept { return options_; }
    [[nodiscard]] std::size_t connection_total() const noexcept { return conn_total_; }
    [[nodiscard]] std::size_t idle_total() const noexcept { return idle_total_; }
    [[nodiscard]] std::size_t group_count() const noexcept { return bucket_index_.size(); }
    [[nodiscard]] bool shutdown_requested() const noexcept;

private:
    friend class Http2PoolAcquireWaiter;
    class DialGuard;
    class JoinAwaiter;
    static Options normalize_options(Options options) noexcept;
    Http2ConnectionPoolGroupBucket *get_bucket(const HttpConnectionGroupKey &key) noexcept;
    Http2ConnectionPoolEntry *allocate_entry(Http2ConnectionPoolGroupBucket &bucket) noexcept;
    void maybe_recycle_bucket(Http2ConnectionPoolGroupBucket &bucket) noexcept;
    void destroy_entry(Http2ConnectionPoolEntry &entry) noexcept;
    void finish_dial(Http2ConnectionPoolEntry &entry, bool success) noexcept;
    bool can_dial(const Http2ConnectionPoolGroupBucket &bucket) const noexcept;
    Http2ConnectionPoolEntry *take_slot(Http2ConnectionPoolGroupBucket &bucket) noexcept;
    void release_slot(Http2ConnectionPoolEntry &entry) noexcept;
    void refresh_capacity(Http2ConnectionPoolEntry &entry) noexcept;
    void set_ready(Http2ConnectionPoolEntry &entry, bool ready) noexcept;
    void set_awaiting_settings(Http2ConnectionPoolEntry &entry, bool awaiting) noexcept;
    std::chrono::milliseconds note_dial_failure(Http2ConnectionPoolGroupBucket &bucket, common::IoErr error) noexcept;
    void remove_idle(Http2ConnectionPoolEntry &entry) noexcept;
    void park_idle(Http2ConnectionPoolEntry &entry) noexcept;
    void retire(Http2ConnectionPoolEntry &entry) noexcept;
    void schedule_maintenance(Http2ConnectionPoolEntry &entry) noexcept;
    static void maintain_entry(Http2ConnectionPoolEntry *entry) noexcept;
    static void on_capacity(void *ctx, Http2Connection &conn) noexcept;
    static void on_closed(void *ctx, Http2Connection &conn, common::IoErr reason) noexcept;
    void wake_waiters(Http2ConnectionPoolGroupBucket &bucket) noexcept;
    void wake_all_groups() noexcept;
    void notify_count(Http2ConnectionPoolGroupBucket &bucket) noexcept;
    void notify_drained() noexcept;
    static void on_retry(Http2ConnectionPoolGroupBucket *bucket) noexcept;
    static void on_expiry(Http2ConnectionPoolCore *pool) noexcept;
    void arm_expiry() noexcept;
    void cancel_expiry() noexcept;

    event::EventLoop *loop_;
    Options options_;
    HttpConnectionBucketIndex bucket_index_{};
    Http2PoolIdleList global_idle_{};
    event::EventLoop::TimerEntry expiry_timer_{};
    Http2ConnectionPoolGroupBucket *free_bucket_head_ = nullptr;
    Http2ConnectionPoolEntry *free_entry_head_ = nullptr;
    std::size_t conn_total_ = 0;
    std::size_t idle_total_ = 0;
    std::size_t acquire_count_ = 0;
    JoinAwaiter *join_head_ = nullptr;
    const std::atomic<bool> *external_shutdown_flag_ = nullptr;
    ConnCountChangedCallback count_cb_ = nullptr;
    void *count_ctx_ = nullptr;
    DialFailedCallback dial_failed_cb_ = nullptr;
    void *dial_failed_ctx_ = nullptr;
    bool shutdown_ = false;
};

} // namespace fiber::http
#endif // FIBER_HTTP_HTTP2_CONNECTION_POOL_CORE_H
