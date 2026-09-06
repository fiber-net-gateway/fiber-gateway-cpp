#include <fiber/http/Http2ConnectionPoolCore.h>

#include <algorithm>
#include <coroutine>
#include <fiber/async/TaskSelect.h>
#include <fiber/async/WhenAny.h>
#include <new>
#include <utility>

namespace fiber::http {
using PoolEntry = Http2ConnectionPoolEntry;
using EntryState = PoolEntry::State;
using IoErr = common::IoErr;
using Clock = std::chrono::steady_clock;

// A waiter pins its bucket for the entire acquire, including a suspended dial.
// Signaling never dequeues it, so a later acquire cannot steal its turn.
class Http2PoolAcquireWaiter {
public:
    Http2PoolAcquireWaiter(Http2ConnectionPoolCore &pool, Http2ConnectionPoolGroupBucket &bucket,
                           Clock::time_point deadline) noexcept : pool_(pool), bucket_(bucket) {
        prev_ = bucket.wait_tail_;
        if (prev_)
            prev_->next_ = this;
        else
            bucket.wait_head_ = this;
        bucket.wait_tail_ = this;
        ++pool_.acquire_count_;
        if (deadline != Clock::time_point::max()) {
            pool_.loop()
                    .post_at<Http2PoolAcquireWaiter, &Http2PoolAcquireWaiter::timer_,
                             &Http2PoolAcquireWaiter::on_timeout>(deadline, *this);
        }
    }
    ~Http2PoolAcquireWaiter() {
        stop_waiting();
        if (timer_.is_in_heap())
            pool_.loop().cancel<Http2PoolAcquireWaiter, &Http2PoolAcquireWaiter::timer_>(*this);
        if (prev_)
            prev_->next_ = next_;
        else
            bucket_.wait_head_ = next_;
        if (next_)
            next_->prev_ = prev_;
        else
            bucket_.wait_tail_ = prev_;
        --pool_.acquire_count_;
        pool_.wake_waiters(bucket_);
        pool_.maybe_recycle_bucket(bucket_);
        pool_.notify_drained();
    }
    class Awaiter {
    public:
        explicit Awaiter(Http2PoolAcquireWaiter &waiter) noexcept : waiter_(waiter) {}
        ~Awaiter() { waiter_.stop_waiting(); }
        bool await_ready() const noexcept { return completed(); }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            waiter_.handle_ = handle;
            return true;
        }
        IoErr await_resume() noexcept {
            waiter_.signaled_ = false;
            return waiter_.result_;
        }
        bool completed() const noexcept { return waiter_.signaled_ || waiter_.result_ != IoErr::None; }

    private:
        Http2PoolAcquireWaiter &waiter_;
    };
    Awaiter wait() noexcept { return Awaiter(*this); }
    void signal(IoErr result = IoErr::None) noexcept {
        if (result != IoErr::None)
            result_ = result;
        signaled_ = true;
        if (handle_ && !posted_) {
            posted_ = true;
            pool_.loop()
                    .post_local<Http2PoolAcquireWaiter, &Http2PoolAcquireWaiter::notify_,
                                &Http2PoolAcquireWaiter::resume>(*this);
        }
    }
    bool has_turn() const noexcept {
        for (auto *waiter = bucket_.wait_head_; waiter != this; waiter = waiter->next_) {
            if (!waiter->dialing_ && waiter->result_ == IoErr::None)
                return false;
        }
        return true;
    }
    void stop_waiting() noexcept {
        if (posted_) {
            pool_.loop().cancel<Http2PoolAcquireWaiter, &Http2PoolAcquireWaiter::notify_>(*this);
            posted_ = false;
        }
        handle_ = {};
    }
    static void resume(Http2PoolAcquireWaiter *waiter) noexcept {
        waiter->posted_ = false;
        auto handle = std::exchange(waiter->handle_, {});
        if (handle)
            handle.resume();
    }
    static void on_timeout(Http2PoolAcquireWaiter *waiter) noexcept { waiter->signal(IoErr::TimedOut); }

    Http2ConnectionPoolCore &pool_;
    Http2ConnectionPoolGroupBucket &bucket_;
    Http2PoolAcquireWaiter *prev_ = nullptr;
    Http2PoolAcquireWaiter *next_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::TimerEntry timer_{};
    event::EventLoop::DeferEntry notify_{};
    IoErr result_ = IoErr::None;
    bool signaled_ = false;
    bool posted_ = false;
    bool dialing_ = false;
};

class Http2ConnectionPoolCore::DialGuard {
public:
    explicit DialGuard(PoolEntry &entry) noexcept : entry_(&entry) {}
    ~DialGuard() {
        if (entry_)
            entry_->pool_->finish_dial(*entry_, false);
    }
    void finish(bool success) noexcept {
        auto *entry = std::exchange(entry_, nullptr);
        entry->pool_->finish_dial(*entry, success);
    }

private:
    PoolEntry *entry_;
};

class Http2ConnectionPoolCore::JoinAwaiter {
public:
    explicit JoinAwaiter(Http2ConnectionPoolCore &pool) noexcept : pool_(pool) {}
    ~JoinAwaiter() {
        if (linked_) {
            auto **pos = &pool_.join_head_;
            while (*pos != this)
                pos = &(*pos)->next_;
            *pos = next_;
        }
        if (posted_)
            pool_.loop().cancel<JoinAwaiter, &JoinAwaiter::notify_>(*this);
    }
    bool await_ready() const noexcept { return pool_.conn_total_ == 0 && pool_.acquire_count_ == 0; }
    void await_suspend(std::coroutine_handle<> handle) noexcept {
        handle_ = handle;
        next_ = pool_.join_head_;
        pool_.join_head_ = this;
        linked_ = true;
    }
    void await_resume() const noexcept {}
    static void resume(JoinAwaiter *waiter) noexcept {
        waiter->posted_ = false;
        auto handle = std::exchange(waiter->handle_, {});
        handle.resume();
    }
    Http2ConnectionPoolCore &pool_;
    JoinAwaiter *next_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::DeferEntry notify_{};
    bool linked_ = false;
    bool posted_ = false;
};

Http2ConnectionPoolCore::Lease::Lease(Lease &&other) noexcept : entry_(std::exchange(other.entry_, nullptr)) {}
Http2ConnectionPoolCore::Lease &Http2ConnectionPoolCore::Lease::operator=(Lease &&other) noexcept {
    if (this != &other) {
        reset();
        entry_ = std::exchange(other.entry_, nullptr);
    }
    return *this;
}
Http2ConnectionPoolCore::Lease::~Lease() { reset(); }
Http2ClientConnection &Http2ConnectionPoolCore::Lease::connection() noexcept {
    FIBER_ASSERT(entry_);
    return entry_->connection();
}
const HttpConnectionGroupKey &Http2ConnectionPoolCore::Lease::key() const noexcept {
    FIBER_ASSERT(entry_);
    return *entry_->pool_->bucket_index_.key_at(entry_->bucket_->slot_index());
}
ClientHttp2Exchange Http2ConnectionPoolCore::Lease::open_exchange(mem::BufPool &pool) noexcept {
    return connection().open_exchange(pool);
}
void Http2ConnectionPoolCore::Lease::reset() noexcept {
    if (auto *entry = std::exchange(entry_, nullptr))
        entry->pool_->release_slot(*entry);
}

Http2ConnectionPoolCore::Options Http2ConnectionPoolCore::normalize_options(Options options) noexcept {
    FIBER_ASSERT(options.idle_timeout > std::chrono::milliseconds::zero());
    FIBER_ASSERT(options.max_connections_per_group >= 1);
    FIBER_ASSERT(options.max_connections_total >= 1);
    FIBER_ASSERT(options.max_concurrent_dials_per_group >= 1);
    FIBER_ASSERT(options.max_idle_total <= options.max_connections_total);
    return options;
}
Http2ConnectionPoolCore::Http2ConnectionPoolCore(event::EventLoop &loop) noexcept :
    Http2ConnectionPoolCore(loop, Options{}) {}
Http2ConnectionPoolCore::Http2ConnectionPoolCore(event::EventLoop &loop, Options options) noexcept :
    loop_(&loop), options_(normalize_options(options)) {}
Http2ConnectionPoolCore::~Http2ConnectionPoolCore() {
    FIBER_ASSERT(conn_total_ == 0 && acquire_count_ == 0 && join_head_ == nullptr);
    FIBER_ASSERT(bucket_index_.empty() && global_idle_.empty());
    FIBER_ASSERT(!expiry_timer_.is_in_heap());
    while (free_entry_head_) {
        auto *entry = free_entry_head_;
        free_entry_head_ = entry->next_free_;
        delete entry;
    }
    while (free_bucket_head_) {
        auto *bucket = free_bucket_head_;
        free_bucket_head_ = bucket->next_free_;
        delete bucket;
    }
}
bool Http2ConnectionPoolCore::init() noexcept { return bucket_index_.init(options_.initial_group_capacity); }
bool Http2ConnectionPoolCore::shutdown_requested() const noexcept {
    return shutdown_ || (external_shutdown_flag_ && external_shutdown_flag_->load(std::memory_order_acquire));
}

Http2ConnectionPoolGroupBucket *Http2ConnectionPoolCore::get_bucket(const HttpConnectionGroupKey &key) noexcept {
    if (auto ref = bucket_index_.find(key))
        return static_cast<Http2ConnectionPoolGroupBucket *>(ref.bucket);
    auto *bucket = free_bucket_head_;
    if (bucket)
        free_bucket_head_ = bucket->next_free_;
    else
        bucket = new (std::nothrow) Http2ConnectionPoolGroupBucket();
    if (!bucket)
        return nullptr;
    bucket->pool_ = this;
    bucket->next_free_ = nullptr;
    if (bucket_index_.insert(key, *bucket) != IoErr::None) {
        bucket->next_free_ = free_bucket_head_;
        free_bucket_head_ = bucket;
        return nullptr;
    }
    return bucket;
}
void Http2ConnectionPoolCore::maybe_recycle_bucket(Http2ConnectionPoolGroupBucket &bucket) noexcept {
    if (bucket.total_count_ || bucket.connecting_count_ || bucket.wait_head_)
        return;
    FIBER_ASSERT(bucket.all_.empty() && bucket.ready_.empty());
    if (bucket.retry_timer_.is_in_heap()) {
        loop_->cancel<Http2ConnectionPoolGroupBucket, &Http2ConnectionPoolGroupBucket::retry_timer_>(bucket);
    }
    bucket_index_.erase(bucket.slot_index());
    bucket.next_free_ = free_bucket_head_;
    free_bucket_head_ = &bucket;
}
PoolEntry *Http2ConnectionPoolCore::allocate_entry(Http2ConnectionPoolGroupBucket &bucket) noexcept {
    auto *entry = free_entry_head_;
    if (entry)
        free_entry_head_ = entry->next_free_;
    else
        entry = new (std::nothrow) PoolEntry();
    if (!entry)
        return nullptr;
    entry->pool_ = this;
    entry->bucket_ = &bucket;
    entry->next_free_ = nullptr;
    entry->state_ = EntryState::Connecting;
    entry->dialing_ = true;
    entry->abort_connection_ = false;
    entry->served_streams_ = 0;
    entry->capacity_cache_ = 0;
    entry->construct_connection(*loop_, options_.h2);
    entry->connection().stream_gate().set_capacity_callback(&Http2ConnectionPoolCore::on_capacity, entry);
    entry->connection().close_gate().add_observer(entry->closed_observer_, &Http2ConnectionPoolCore::on_closed, entry);
    bucket.all_.push_back(*entry);
    ++bucket.total_count_;
    ++bucket.connecting_count_;
    ++conn_total_;
    notify_count(bucket);
    return entry;
}
bool Http2ConnectionPoolCore::can_dial(const Http2ConnectionPoolGroupBucket &bucket) const noexcept {
    return !bucket.retry_timer_.is_in_heap() && bucket.total_count_ < options_.max_connections_per_group &&
           bucket.connecting_count_ < options_.max_concurrent_dials_per_group &&
           conn_total_ < options_.max_connections_total;
}

async::Task<common::IoResult<Http2ConnectionPoolCore::Lease>>
Http2ConnectionPoolCore::acquire(HttpConnectionGroupKey key, Connector connector,
                                 std::chrono::milliseconds timeout) noexcept {
    FIBER_ASSERT(loop_->in_loop());
    if (shutdown_requested())
        co_return std::unexpected(IoErr::Canceled);
    const bool poll = timeout <= std::chrono::milliseconds::zero();
    const auto deadline =
            poll || timeout == std::chrono::milliseconds::max()
                    ? Clock::time_point::max()
                    : loop_->now() + std::chrono::duration_cast<Clock::duration>(
                                             std::min(timeout, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                       Clock::time_point::max() - loop_->now())));
    auto *bucket = get_bucket(key);
    if (!bucket)
        co_return std::unexpected(IoErr::NoMem);
    // A reusable connection with no queued competitors never arms a timer or
    // links a waiter on the request hot path.
    if (!bucket->wait_head_) {
        if (auto *entry = take_slot(*bucket))
            co_return Lease(*entry);
        if (poll) {
            maybe_recycle_bucket(*bucket);
            co_return std::unexpected(IoErr::Busy);
        }
    }
    Http2PoolAcquireWaiter waiter(*this, *bucket, deadline);
    for (;;) {
        if (shutdown_requested())
            co_return std::unexpected(IoErr::Canceled);
        if (waiter.result_ != IoErr::None)
            co_return std::unexpected(waiter.result_);
        if (deadline != Clock::time_point::max() && loop_->now() >= deadline)
            co_return std::unexpected(IoErr::TimedOut);
        if (waiter.has_turn()) {
            if (auto *entry = take_slot(*bucket))
                co_return Lease(*entry);
            if (!poll && can_dial(*bucket)) {
                if (!connector.connect)
                    co_return std::unexpected(IoErr::Invalid);
                auto *entry = allocate_entry(*bucket);
                if (!entry)
                    co_return std::unexpected(IoErr::NoMem);
                DialGuard guard(*entry);
                waiter.dialing_ = true;
                waiter.signaled_ = false;
                wake_waiters(*bucket);
                auto result = co_await async::when_any(
                        [&]() { return connector.connect(connector.ctx, entry->connection(), key).select(); },
                        [&]() { return waiter.wait(); });
                waiter.dialing_ = false;
                bool success = result.is<0>() && result.get<0>().has_value() && waiter.result_ == IoErr::None &&
                               !shutdown_requested() && entry->state_ == EntryState::Connecting;
                // A connector must actually start the client session before returning success.
                const auto state = entry->connection().http2().state();
                success =
                        success && (state == Http2Connection::State::Start || state == Http2Connection::State::Running);
                guard.finish(success);
                if (success) {
                    // The dial owner gets first use; queued callers share the remainder.
                    if (auto *taken = take_slot(*bucket))
                        co_return Lease(*taken);
                    if (entry->state_ == EntryState::Ready)
                        park_idle(*entry);
                } else if (waiter.result_ == IoErr::None && !shutdown_requested() &&
                           !bucket->retry_timer_.is_in_heap()) {
                    loop_->post_at<Http2ConnectionPoolGroupBucket, &Http2ConnectionPoolGroupBucket::retry_timer_,
                                   &Http2ConnectionPoolCore::on_retry>(loop_->now() + std::chrono::milliseconds(10),
                                                                       *bucket);
                }
                continue;
            }
        }
        if (poll)
            co_return std::unexpected(IoErr::Busy);
        (void) co_await waiter.wait();
    }
}

void Http2ConnectionPoolCore::finish_dial(PoolEntry &entry, bool success) noexcept {
    FIBER_ASSERT(entry.dialing_);
    entry.dialing_ = false;
    --entry.bucket_->connecting_count_;
    if (success) {
        entry.state_ = EntryState::Ready;
        refresh_capacity(entry);
    } else {
        entry.abort_connection_ = true;
        retire(entry);
    }
    wake_waiters(*entry.bucket_);
}
void Http2ConnectionPoolCore::set_ready(PoolEntry &entry, bool ready) noexcept {
    if (ready == entry.ready_hook_.linked())
        return;
    auto &bucket = *entry.bucket_;
    if (ready) {
        bucket.ready_.push_front(entry);
        ++bucket.ready_count_;
    } else {
        bucket.ready_.erase(entry);
        --bucket.ready_count_;
    }
    notify_count(bucket);
}
void Http2ConnectionPoolCore::refresh_capacity(PoolEntry &entry) noexcept {
    if (entry.state_ != EntryState::Ready)
        return;
    const auto &conn = entry.connection().http2();
    const auto status = conn.local_stream_attach_status();
    if (status != IoErr::None && status != IoErr::Busy) {
        retire(entry);
        return;
    }
    std::size_t capacity =
            conn.peer_settings_received() ? conn.peer_max_concurrent_streams() : options_.pre_settings_max_streams;
    if (options_.max_streams_per_connection)
        capacity = std::min(capacity, options_.max_streams_per_connection);
    entry.capacity_cache_ = capacity;
    set_ready(entry, entry.active_leases_ < capacity && conn.accepts_new_local_stream());
}
PoolEntry *Http2ConnectionPoolCore::take_slot(Http2ConnectionPoolGroupBucket &bucket) noexcept {
    while (auto *entry = bucket.ready_.front()) {
        refresh_capacity(*entry);
        if (!entry->ready_hook_.linked())
            continue;
        remove_idle(*entry);
        ++entry->active_leases_;
        if (entry->active_leases_ >= entry->capacity_cache_)
            set_ready(*entry, false);
        return entry;
    }
    return nullptr;
}
void Http2ConnectionPoolCore::release_slot(PoolEntry &entry) noexcept {
    FIBER_ASSERT(loop_->in_loop() && entry.active_leases_ > 0);
    --entry.active_leases_;
    if (entry.served_streams_ != UINT64_MAX)
        ++entry.served_streams_;
    if (shutdown_requested() ||
        (options_.max_streams_lifetime && entry.served_streams_ >= options_.max_streams_lifetime)) {
        retire(entry);
    } else
        refresh_capacity(entry);
    if (entry.active_leases_ == 0) {
        if (entry.state_ == EntryState::Ready)
            park_idle(entry);
        else
            schedule_maintenance(entry);
    }
    wake_waiters(*entry.bucket_);
}
void Http2ConnectionPoolCore::remove_idle(PoolEntry &entry) noexcept {
    if (!entry.idle_hook_.linked())
        return;
    global_idle_.erase(entry);
    --idle_total_;
    if (global_idle_.empty())
        cancel_expiry();
}
void Http2ConnectionPoolCore::park_idle(PoolEntry &entry) noexcept {
    FIBER_ASSERT(entry.active_leases_ == 0 && entry.state_ == EntryState::Ready);
    if (entry.idle_hook_.linked())
        return;
    entry.idle_since_ = loop_->now();
    global_idle_.push_back(entry);
    ++idle_total_;
    while (idle_total_ > options_.max_idle_total)
        retire(*global_idle_.front());
    arm_expiry();
}
void Http2ConnectionPoolCore::retire(PoolEntry &entry) noexcept {
    set_ready(entry, false);
    remove_idle(entry);
    if (entry.state_ != EntryState::Closed)
        entry.state_ = EntryState::Draining;
    if (!entry.active_leases_ && !entry.dialing_)
        schedule_maintenance(entry);
}
void Http2ConnectionPoolCore::schedule_maintenance(PoolEntry &entry) noexcept {
    if (entry.maintenance_posted_)
        return;
    entry.maintenance_posted_ = true;
    loop_->post_local<PoolEntry, &PoolEntry::maintenance_entry_, &Http2ConnectionPoolCore::maintain_entry>(entry);
}
void Http2ConnectionPoolCore::maintain_entry(PoolEntry *entry) noexcept {
    entry->maintenance_posted_ = false;
    if (entry->active_leases_ || entry->dialing_)
        return;
    if (entry->state_ == EntryState::Closed) {
        entry->pool_->destroy_entry(*entry);
        return;
    }
    if (entry->state_ != EntryState::Draining)
        return;
    auto &client = entry->connection();
    if (entry->abort_connection_ || client.http2().state() == Http2Connection::State::Init)
        client.shutdown();
    else
        client.http2().graceful_shutdown();
    // Init/start failure can mark closure without dispatching the callback.
    if (client.close_gate().closed())
        on_closed(entry, client.http2(), client.http2().terminal_error());
}
void Http2ConnectionPoolCore::on_capacity(void *ctx, Http2Connection &) noexcept {
    auto &entry = *static_cast<PoolEntry *>(ctx);
    if (entry.dialing_)
        return;
    entry.pool_->refresh_capacity(entry);
    entry.pool_->wake_waiters(*entry.bucket_);
}
void Http2ConnectionPoolCore::on_closed(void *ctx, Http2Connection &, IoErr) noexcept {
    auto &entry = *static_cast<PoolEntry *>(ctx);
    entry.state_ = EntryState::Closed;
    entry.pool_->retire(entry);
    entry.pool_->wake_waiters(*entry.bucket_);
}
void Http2ConnectionPoolCore::destroy_entry(PoolEntry &entry) noexcept {
    FIBER_ASSERT(entry.state_ == EntryState::Closed && !entry.active_leases_ && !entry.dialing_);
    auto &bucket = *entry.bucket_;
    entry.connection().stream_gate().clear_capacity_callback();
    entry.destroy_connection();
    bucket.all_.erase(entry);
    --bucket.total_count_;
    --conn_total_;
    entry.bucket_ = nullptr;
    entry.state_ = EntryState::Free;
    entry.next_free_ = free_entry_head_;
    free_entry_head_ = &entry;
    notify_count(bucket);
    maybe_recycle_bucket(bucket);
    // A global connection limit may have blocked a completely different key.
    wake_all_groups();
    notify_drained();
}
void Http2ConnectionPoolCore::wake_waiters(Http2ConnectionPoolGroupBucket &bucket) noexcept {
    if (shutdown_requested())
        return;
    if (bucket.ready_.empty() && !can_dial(bucket))
        return;
    for (auto *waiter = bucket.wait_head_; waiter; waiter = waiter->next_) {
        if (waiter->dialing_ || waiter->result_ != IoErr::None)
            continue;
        waiter->signal();
        break;
    }
}
void Http2ConnectionPoolCore::wake_all_groups() noexcept {
    for (std::size_t i = 0; i < bucket_index_.slot_capacity(); ++i) {
        if (auto *bucket = bucket_index_.bucket_at(static_cast<std::uint32_t>(i))) {
            wake_waiters(*static_cast<Http2ConnectionPoolGroupBucket *>(bucket));
        }
    }
}
void Http2ConnectionPoolCore::notify_count(Http2ConnectionPoolGroupBucket &bucket) noexcept {
    if (count_cb_)
        count_cb_(count_ctx_, *bucket_index_.key_at(bucket.slot_index()), bucket.total_count_, bucket.ready_count_);
}
void Http2ConnectionPoolCore::notify_drained() noexcept {
    if (conn_total_ || acquire_count_)
        return;
    while (join_head_) {
        auto *waiter = join_head_;
        join_head_ = waiter->next_;
        waiter->linked_ = false;
        waiter->posted_ = true;
        loop_->post_local<JoinAwaiter, &JoinAwaiter::notify_, &JoinAwaiter::resume>(*waiter);
    }
}
void Http2ConnectionPoolCore::on_retry(Http2ConnectionPoolGroupBucket *bucket) noexcept {
    bucket->pool_->wake_waiters(*bucket);
}
void Http2ConnectionPoolCore::cancel_expiry() noexcept {
    if (expiry_timer_.is_in_heap())
        loop_->cancel<Http2ConnectionPoolCore, &Http2ConnectionPoolCore::expiry_timer_>(*this);
}
void Http2ConnectionPoolCore::arm_expiry() noexcept {
    if (expiry_timer_.is_in_heap() || global_idle_.empty())
        return;
    loop_->post_at<Http2ConnectionPoolCore, &Http2ConnectionPoolCore::expiry_timer_,
                   &Http2ConnectionPoolCore::on_expiry>(global_idle_.front()->idle_since_ + options_.idle_timeout,
                                                        *this);
}
void Http2ConnectionPoolCore::on_expiry(Http2ConnectionPoolCore *pool) noexcept {
    const auto now = pool->loop_->now();
    while (auto *entry = pool->global_idle_.front()) {
        if (now - entry->idle_since_ < pool->options_.idle_timeout)
            break;
        pool->retire(*entry);
    }
    pool->arm_expiry();
}
void Http2ConnectionPoolCore::clear() noexcept {
    if (!conn_total_ && !acquire_count_)
        return;
    FIBER_ASSERT(loop_->in_loop());
    cancel_expiry();
    for (std::size_t i = 0; i < bucket_index_.slot_capacity(); ++i) {
        auto *bucket =
                static_cast<Http2ConnectionPoolGroupBucket *>(bucket_index_.bucket_at(static_cast<std::uint32_t>(i)));
        if (!bucket)
            continue;
        if (bucket->retry_timer_.is_in_heap())
            loop_->cancel<Http2ConnectionPoolGroupBucket, &Http2ConnectionPoolGroupBucket::retry_timer_>(*bucket);
        for (auto *waiter = bucket->wait_head_; waiter; waiter = waiter->next_)
            waiter->signal(IoErr::Canceled);
        for (auto *entry = bucket->all_.front(); entry; entry = bucket->all_.next_of(*entry))
            retire(*entry);
    }
}
void Http2ConnectionPoolCore::shutdown() noexcept {
    shutdown_ = true;
    clear();
}
async::Task<void> Http2ConnectionPoolCore::join() noexcept {
    FIBER_ASSERT(loop_->in_loop());
    co_await JoinAwaiter(*this);
}
} // namespace fiber::http
