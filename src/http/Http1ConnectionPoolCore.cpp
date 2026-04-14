#include "Http1ConnectionPoolCore.h"

#include <new>
#include <utility>

#include "../common/Assert.h"

namespace fiber::http {

Http1ConnectionPoolCore::Lease::Lease(Http1ConnectionPoolCore &pool, Http1ConnectionPoolEntry *entry,
                                      const Http1ConnectionGroupKey &key, bool hit) noexcept :
    pool_(&pool), entry_(entry), key_(key), hit_(hit) {}

Http1ConnectionPoolCore::Lease::Lease(Lease &&other) noexcept :
    pool_(other.pool_), entry_(other.entry_), key_(std::move(other.key_)), hit_(other.hit_) {
    other.pool_ = nullptr;
    other.entry_ = nullptr;
    other.key_.reset();
    other.hit_ = false;
}

Http1ConnectionPoolCore::Lease &Http1ConnectionPoolCore::Lease::operator=(Lease &&other) noexcept {
    if (this == &other) {
        return *this;
    }
    reset();
    pool_ = other.pool_;
    entry_ = other.entry_;
    key_ = std::move(other.key_);
    hit_ = other.hit_;
    other.pool_ = nullptr;
    other.entry_ = nullptr;
    other.key_.reset();
    other.hit_ = false;
    return *this;
}

Http1ConnectionPoolCore::Lease::~Lease() { reset(); }

Http1ClientConnection &Http1ConnectionPoolCore::Lease::connection() noexcept {
    FIBER_ASSERT(entry_ != nullptr);
    Http1ClientConnection *conn = entry_->connection();
    FIBER_ASSERT(conn != nullptr);
    return *conn;
}

const Http1ConnectionGroupKey &Http1ConnectionPoolCore::Lease::key() const noexcept {
    FIBER_ASSERT(key_.has_value());
    return *key_;
}

common::IoResult<Http1ClientConnection *>
Http1ConnectionPoolCore::Lease::emplace_connection(Http1ClientConnectionOptions options) noexcept {
    if (!pool_ || !key_.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (pool_->shutdown_effective()) {
        return std::unexpected(common::IoErr::Canceled);
    }
    if (!entry_) {
        entry_ = pool_->allocate_entry();
        if (!entry_) {
            return std::unexpected(common::IoErr::NoMem);
        }
    }
    if (entry_->has_connection()) {
        return entry_->connection();
    }
    entry_->construct_connection(pool_->loop(), std::move(options));
    return entry_->connection();
}

void Http1ConnectionPoolCore::Lease::reset() noexcept {
    if (!pool_) {
        pool_ = nullptr;
        entry_ = nullptr;
        key_.reset();
        hit_ = false;
        return;
    }
    if (entry_) {
        pool_->release_lease(*this);
    }
    pool_ = nullptr;
    entry_ = nullptr;
    key_.reset();
    hit_ = false;
}

Http1ConnectionPoolCore::Options Http1ConnectionPoolCore::normalize_options(Options options) noexcept {
    FIBER_ASSERT(options.idle_timeout > std::chrono::milliseconds::zero());
    if (options.max_idle_total == 0) {
        options.max_idle_per_group = 0;
    } else if (options.max_idle_per_group > options.max_idle_total) {
        options.max_idle_per_group = options.max_idle_total;
    }
    return options;
}

Http1ConnectionPoolCore::Http1ConnectionPoolCore(event::EventLoop &loop, Options options) noexcept :
    loop_(&loop), options_(normalize_options(options)) {}

Http1ConnectionPoolCore::Http1ConnectionPoolCore(event::EventLoop &loop) noexcept :
    Http1ConnectionPoolCore(loop, Options{}) {}

Http1ConnectionPoolCore::~Http1ConnectionPoolCore() {
    clear();
    destroy_free_lists();
}

bool Http1ConnectionPoolCore::shutdown_requested() const noexcept { return shutdown_effective(); }

bool Http1ConnectionPoolCore::shutdown_effective() const noexcept {
    if (shutdown_) {
        return true;
    }
    return external_shutdown_flag_ != nullptr && external_shutdown_flag_->load(std::memory_order_acquire);
}

bool Http1ConnectionPoolCore::init() noexcept { return bucket_index_.init(options_.initial_group_capacity); }

Http1ConnectionPoolCore::Lease Http1ConnectionPoolCore::acquire(const Http1ConnectionGroupKey &key) noexcept {
    if (shutdown_effective()) {
        return {};
    }
    Http1ConnectionPoolEntry *entry = try_steal_idle_entry(key);
    if (entry) {
        return Lease(*this, entry, key, true);
    }
    return Lease(*this, nullptr, key, false);
}

Http1ConnectionPoolEntry *Http1ConnectionPoolCore::try_steal_idle_entry(const Http1ConnectionGroupKey &key) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (shutdown_effective()) {
        return nullptr;
    }

    for (;;) {
        auto entry_ref = bucket_index_.find(key);
        if (!entry_ref) {
            break;
        }

        Http1ConnectionPoolGroupBucket *bucket = entry_ref.bucket;
        if (!bucket || bucket->idle_entries_.empty()) {
            if (bucket && bucket->slot_index_ != Http1ConnectionPoolGroupBucket::kInvalidSlotIndex) {
                bucket_index_.erase(bucket->slot_index_);
                recycle_bucket(bucket);
            }
            break;
        }

        Http1ConnectionPoolEntry *entry = bucket->idle_entries_.back();
        FIBER_ASSERT(entry != nullptr);
        FIBER_ASSERT(entry->has_connection());
        detach_idle_entry(*entry);
        return entry;
    }
    return nullptr;
}

void Http1ConnectionPoolCore::accept_returned_entry(Http1ConnectionPoolEntry &entry,
                                                    const Http1ConnectionGroupKey &key) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (shutdown_effective()) {
        recycle_entry(&entry);
        return;
    }
    park_entry(entry, key);
}

void Http1ConnectionPoolCore::shutdown() noexcept {
    shutdown_ = true;
    clear();
}

void Http1ConnectionPoolCore::on_expiry_timer(Http1ConnectionPoolCore *pool) noexcept {
    FIBER_ASSERT(pool != nullptr);
    pool->on_expiry_timer_fired();
}

void Http1ConnectionPoolCore::arm_expiry_timer(std::chrono::steady_clock::time_point when) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    loop_->post_at<Http1ConnectionPoolCore, &Http1ConnectionPoolCore::expiry_timer_,
                   &Http1ConnectionPoolCore::on_expiry_timer>(when, *this);
}

void Http1ConnectionPoolCore::arm_expiry_timer_if_needed() noexcept {
    if (expiry_timer_.is_in_heap() || global_idle_entries_.empty()) {
        return;
    }
    Http1ConnectionPoolEntry *entry = global_idle_entries_.front();
    FIBER_ASSERT(entry != nullptr);
    arm_expiry_timer(entry->idle_since_ + options_.idle_timeout);
}

void Http1ConnectionPoolCore::cancel_expiry_timer() noexcept {
    if (!expiry_timer_.is_in_heap()) {
        return;
    }
    FIBER_ASSERT(loop_ != nullptr);
    if (loop_->in_loop()) {
        loop_->cancel<Http1ConnectionPoolCore, &Http1ConnectionPoolCore::expiry_timer_>(*this);
        return;
    }
    loop_->cancel_quiesced<Http1ConnectionPoolCore, &Http1ConnectionPoolCore::expiry_timer_>(*this);
}

void Http1ConnectionPoolCore::on_expiry_timer_fired() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!expiry_timer_.is_in_heap());
    evict_expired_entries(loop_->now());
    arm_expiry_timer_if_needed();
}

void Http1ConnectionPoolCore::evict_expired_entries(std::chrono::steady_clock::time_point now) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    while (!global_idle_entries_.empty()) {
        Http1ConnectionPoolEntry *entry = global_idle_entries_.front();
        FIBER_ASSERT(entry != nullptr);
        if (!entry_expired(*entry, now)) {
            break;
        }
        evict_entry(*entry);
    }
}

void Http1ConnectionPoolCore::clear() noexcept {
    cancel_expiry_timer();
    while (!global_idle_entries_.empty()) {
        Http1ConnectionPoolEntry *entry = global_idle_entries_.front();
        FIBER_ASSERT(entry != nullptr);
        evict_entry(*entry);
    }
    bucket_index_.clear();
    idle_total_ = 0;
}

bool Http1ConnectionPoolCore::entry_expired(const Http1ConnectionPoolEntry &entry,
                                            std::chrono::steady_clock::time_point now) const noexcept {
    return now - entry.idle_since_ >= options_.idle_timeout;
}

Http1ConnectionPoolGroupBucket *Http1ConnectionPoolCore::allocate_bucket() noexcept {
    if (free_bucket_head_) {
        auto *bucket = free_bucket_head_;
        free_bucket_head_ = free_bucket_head_->next_free_;
        bucket->slot_index_ = Http1ConnectionPoolGroupBucket::kInvalidSlotIndex;
        bucket->idle_count_ = 0;
        bucket->next_free_ = nullptr;
        return bucket;
    }
    return new (std::nothrow) Http1ConnectionPoolGroupBucket();
}

Http1ConnectionPoolEntry *Http1ConnectionPoolCore::allocate_entry() noexcept {
    if (free_entry_head_) {
        auto *entry = free_entry_head_;
        free_entry_head_ = free_entry_head_->next_free_;
        entry->bucket_ = nullptr;
        entry->idle_since_ = {};
        entry->next_free_ = nullptr;
        entry->clear_remote_return_state();
        return entry;
    }
    return new (std::nothrow) Http1ConnectionPoolEntry();
}

void Http1ConnectionPoolCore::recycle_bucket(Http1ConnectionPoolGroupBucket *bucket) noexcept {
    if (!bucket) {
        return;
    }
    bucket->slot_index_ = Http1ConnectionPoolGroupBucket::kInvalidSlotIndex;
    bucket->idle_count_ = 0;
    bucket->next_free_ = free_bucket_head_;
    free_bucket_head_ = bucket;
}

void Http1ConnectionPoolCore::recycle_entry(Http1ConnectionPoolEntry *entry) noexcept {
    if (!entry) {
        return;
    }
    entry->destroy_connection();
    entry->clear_remote_return_state();
    entry->bucket_ = nullptr;
    entry->idle_since_ = {};
    entry->next_free_ = free_entry_head_;
    free_entry_head_ = entry;
}

void Http1ConnectionPoolCore::destroy_free_lists() noexcept {
    while (free_entry_head_) {
        Http1ConnectionPoolEntry *next = free_entry_head_->next_free_;
        delete free_entry_head_;
        free_entry_head_ = next;
    }
    while (free_bucket_head_) {
        Http1ConnectionPoolGroupBucket *next = free_bucket_head_->next_free_;
        delete free_bucket_head_;
        free_bucket_head_ = next;
    }
}

void Http1ConnectionPoolCore::park_entry(Http1ConnectionPoolEntry &entry, const Http1ConnectionGroupKey &key) noexcept {
    if (!entry.has_connection()) {
        recycle_entry(&entry);
        return;
    }
    if (shutdown_effective()) {
        recycle_entry(&entry);
        return;
    }

    Http1ClientConnection *conn = entry.connection();
    FIBER_ASSERT(conn != nullptr);
    if (options_.max_idle_total == 0 || options_.max_idle_per_group == 0 || &conn->loop() != loop_ ||
        !conn->reusable()) {
        recycle_entry(&entry);
        return;
    }

    const auto now = loop_->now();
    Http1ConnectionPoolGroupBucket *bucket = nullptr;
    auto entry_ref = bucket_index_.find(key);
    if (entry_ref) {
        bucket = entry_ref.bucket;
    } else {
        bucket = allocate_bucket();
        if (!bucket) {
            recycle_entry(&entry);
            return;
        }

        const common::IoErr insert_err = bucket_index_.insert(key, *bucket);
        if (insert_err != common::IoErr::None) {
            recycle_bucket(bucket);
            recycle_entry(&entry);
            return;
        }
    }

    entry.bucket_ = bucket;
    entry.idle_since_ = now;
    bucket->idle_entries_.push_back(entry);
    global_idle_entries_.push_back(entry);
    ++bucket->idle_count_;
    ++idle_total_;
    if (idle_count_changed_cb_) {
        idle_count_changed_cb_(idle_count_changed_ctx_, key, bucket->idle_count_);
    }
    while (bucket->idle_count_ > options_.max_idle_per_group) {
        evict_group_oldest(*bucket);
    }
    while (idle_total_ > options_.max_idle_total) {
        evict_global_oldest();
    }
    arm_expiry_timer_if_needed();
}

void Http1ConnectionPoolCore::release_lease(Lease &lease) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(lease.entry_ != nullptr);
    FIBER_ASSERT(lease.key_.has_value());
    park_entry(*lease.entry_, *lease.key_);
}

void Http1ConnectionPoolCore::detach_idle_entry(Http1ConnectionPoolEntry &entry) noexcept {
    Http1ConnectionPoolGroupBucket *bucket = entry.bucket_;
    FIBER_ASSERT(bucket != nullptr);
    FIBER_ASSERT(bucket->idle_entries_.back() != nullptr);
    const Http1ConnectionGroupKey *key = bucket->slot_index_ != Http1ConnectionPoolGroupBucket::kInvalidSlotIndex
                                                 ? bucket_index_.key_at(bucket->slot_index_)
                                                 : nullptr;

    bucket->idle_entries_.erase(entry);
    global_idle_entries_.erase(entry);
    entry.bucket_ = nullptr;

    FIBER_ASSERT(bucket->idle_count_ > 0);
    --bucket->idle_count_;
    FIBER_ASSERT(idle_total_ > 0);
    --idle_total_;
    if (idle_count_changed_cb_ && key) {
        idle_count_changed_cb_(idle_count_changed_ctx_, *key, bucket->idle_count_);
    }
    if (bucket->idle_count_ == 0 && bucket->slot_index_ != Http1ConnectionPoolGroupBucket::kInvalidSlotIndex) {
        bucket_index_.erase(bucket->slot_index_);
        recycle_bucket(bucket);
    }
}

void Http1ConnectionPoolCore::evict_entry(Http1ConnectionPoolEntry &entry) noexcept {
    if (entry.group_hook_.linked() || entry.global_hook_.linked()) {
        detach_idle_entry(entry);
    }
    recycle_entry(&entry);
}

void Http1ConnectionPoolCore::evict_group_oldest(Http1ConnectionPoolGroupBucket &bucket) noexcept {
    Http1ConnectionPoolEntry *entry = bucket.idle_entries_.front();
    if (entry) {
        evict_entry(*entry);
    }
}

void Http1ConnectionPoolCore::evict_global_oldest() noexcept {
    Http1ConnectionPoolEntry *entry = global_idle_entries_.front();
    if (entry) {
        evict_entry(*entry);
    }
}

} // namespace fiber::http
