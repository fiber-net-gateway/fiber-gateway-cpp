#include "Http1ConnectionPool.h"

#include <new>
#include <utility>

#include "../common/Assert.h"

namespace fiber::http {

Http1ConnectionPool::Lease::Lease(Http1ConnectionPool &pool,
                                  Http1ConnectionPoolEntry *entry,
                                  const Http1ConnectionGroupKey &key,
                                  bool hit) noexcept
    : pool_(&pool),
      entry_(entry),
      key_(key),
      hit_(hit) {}

Http1ConnectionPool::Lease::Lease(Lease &&other) noexcept
    : pool_(other.pool_),
      entry_(other.entry_),
      key_(std::move(other.key_)),
      hit_(other.hit_) {
    other.pool_ = nullptr;
    other.entry_ = nullptr;
    other.key_.reset();
    other.hit_ = false;
}

Http1ConnectionPool::Lease &Http1ConnectionPool::Lease::operator=(Lease &&other) noexcept {
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

Http1ConnectionPool::Lease::~Lease() { reset(); }

Http1ClientConnection &Http1ConnectionPool::Lease::connection() noexcept {
    FIBER_ASSERT(entry_ != nullptr);
    Http1ClientConnection *conn = entry_->connection();
    FIBER_ASSERT(conn != nullptr);
    return *conn;
}

const Http1ConnectionGroupKey &Http1ConnectionPool::Lease::key() const noexcept {
    FIBER_ASSERT(key_.has_value());
    return *key_;
}

common::IoResult<Http1ClientConnection *> Http1ConnectionPool::Lease::emplace_connection(
    Http1ClientConnectionOptions options) noexcept {
    if (!pool_ || !key_.has_value()) {
        return std::unexpected(common::IoErr::Invalid);
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

void Http1ConnectionPool::Lease::reset() noexcept {
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

Http1ConnectionPool::Options Http1ConnectionPool::normalize_options(Options options) noexcept {
    if (options.max_idle_total == 0) {
        options.max_idle_per_group = 0;
    } else if (options.max_idle_per_group > options.max_idle_total) {
        options.max_idle_per_group = options.max_idle_total;
    }
    return options;
}

Http1ConnectionPool::Http1ConnectionPool(event::EventLoop &loop, Options options) noexcept
    : loop_(&loop),
      options_(normalize_options(options)) {}

Http1ConnectionPool::Http1ConnectionPool(event::EventLoop &loop) noexcept
    : Http1ConnectionPool(loop, Options{}) {}

Http1ConnectionPool::~Http1ConnectionPool() {
    clear();
    destroy_free_lists();
}

bool Http1ConnectionPool::init() noexcept { return bucket_index_.init(options_.initial_group_capacity); }

Http1ConnectionPool::Lease Http1ConnectionPool::acquire(const Http1ConnectionGroupKey &key) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());

    const auto now = loop_->now();
    sweep_expired(now);

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
        if (entry_expired(*entry, now) || !entry_reusable(*entry)) {
            evict_entry(*entry);
            continue;
        }
        detach_idle_entry(*entry);
        return Lease(*this, entry, key, true);
    }
    return Lease(*this, nullptr, key, false);
}

void Http1ConnectionPool::sweep_expired(std::chrono::steady_clock::time_point now) noexcept {
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

void Http1ConnectionPool::clear() noexcept {
    while (!global_idle_entries_.empty()) {
        Http1ConnectionPoolEntry *entry = global_idle_entries_.front();
        FIBER_ASSERT(entry != nullptr);
        evict_entry(*entry);
    }
    bucket_index_.clear();
    hint_table_.clear();
    idle_total_ = 0;
}

bool Http1ConnectionPool::entry_expired(const Http1ConnectionPoolEntry &entry,
                                        std::chrono::steady_clock::time_point now) const noexcept {
    if (options_.idle_timeout <= std::chrono::milliseconds::zero()) {
        return true;
    }
    return now - entry.idle_since_ >= options_.idle_timeout;
}

bool Http1ConnectionPool::entry_reusable(const Http1ConnectionPoolEntry &entry) const noexcept {
    const Http1ClientConnection *conn = entry.connection();
    return conn && conn->reusable();
}

Http1ConnectionPoolGroupBucket *Http1ConnectionPool::allocate_bucket() noexcept {
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

Http1ConnectionPoolEntry *Http1ConnectionPool::allocate_entry() noexcept {
    if (free_entry_head_) {
        auto *entry = free_entry_head_;
        free_entry_head_ = free_entry_head_->next_free_;
        entry->bucket_ = nullptr;
        entry->idle_since_ = {};
        entry->next_free_ = nullptr;
        return entry;
    }
    return new (std::nothrow) Http1ConnectionPoolEntry();
}

void Http1ConnectionPool::recycle_bucket(Http1ConnectionPoolGroupBucket *bucket) noexcept {
    if (!bucket) {
        return;
    }
    bucket->slot_index_ = Http1ConnectionPoolGroupBucket::kInvalidSlotIndex;
    bucket->idle_count_ = 0;
    bucket->next_free_ = free_bucket_head_;
    free_bucket_head_ = bucket;
}

void Http1ConnectionPool::recycle_entry(Http1ConnectionPoolEntry *entry) noexcept {
    if (!entry) {
        return;
    }
    entry->destroy_connection();
    entry->bucket_ = nullptr;
    entry->idle_since_ = {};
    entry->next_free_ = free_entry_head_;
    free_entry_head_ = entry;
}

void Http1ConnectionPool::destroy_free_lists() noexcept {
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

void Http1ConnectionPool::park_entry(Http1ConnectionPoolEntry &entry, const Http1ConnectionGroupKey &key) noexcept {
    if (!entry.has_connection()) {
        recycle_entry(&entry);
        return;
    }

    Http1ClientConnection *conn = entry.connection();
    FIBER_ASSERT(conn != nullptr);
    if (options_.max_idle_total == 0 || options_.max_idle_per_group == 0 || &conn->loop() != loop_ || !conn->reusable()) {
        recycle_entry(&entry);
        return;
    }

    const auto now = loop_->now();
    sweep_expired(now);

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
    hint_table_.note_idle_add(key);

    while (bucket->idle_count_ > options_.max_idle_per_group) {
        evict_group_oldest(*bucket);
    }
    while (idle_total_ > options_.max_idle_total) {
        evict_global_oldest();
    }
}

void Http1ConnectionPool::release_lease(Lease &lease) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(lease.entry_ != nullptr);
    FIBER_ASSERT(lease.key_.has_value());
    park_entry(*lease.entry_, *lease.key_);
}

void Http1ConnectionPool::detach_idle_entry(Http1ConnectionPoolEntry &entry) noexcept {
    Http1ConnectionPoolGroupBucket *bucket = entry.bucket_;
    FIBER_ASSERT(bucket != nullptr);
    FIBER_ASSERT(bucket->idle_entries_.back() != nullptr);

    bucket->idle_entries_.erase(entry);
    global_idle_entries_.erase(entry);
    entry.bucket_ = nullptr;

    FIBER_ASSERT(bucket->idle_count_ > 0);
    --bucket->idle_count_;
    FIBER_ASSERT(idle_total_ > 0);
    --idle_total_;
    if (bucket->slot_index_ != Http1ConnectionPoolGroupBucket::kInvalidSlotIndex) {
        const Http1ConnectionGroupKey *key = bucket_index_.key_at(bucket->slot_index_);
        if (key) {
            hint_table_.note_idle_remove(*key);
        }
    }

    if (bucket->idle_count_ == 0 && bucket->slot_index_ != Http1ConnectionPoolGroupBucket::kInvalidSlotIndex) {
        bucket_index_.erase(bucket->slot_index_);
        recycle_bucket(bucket);
    }
}

void Http1ConnectionPool::evict_entry(Http1ConnectionPoolEntry &entry) noexcept {
    if (entry.group_hook_.linked() || entry.global_hook_.linked()) {
        detach_idle_entry(entry);
    }
    recycle_entry(&entry);
}

void Http1ConnectionPool::evict_group_oldest(Http1ConnectionPoolGroupBucket &bucket) noexcept {
    Http1ConnectionPoolEntry *entry = bucket.idle_entries_.front();
    if (entry) {
        evict_entry(*entry);
    }
}

void Http1ConnectionPool::evict_global_oldest() noexcept {
    Http1ConnectionPoolEntry *entry = global_idle_entries_.front();
    if (entry) {
        evict_entry(*entry);
    }
}

} // namespace fiber::http
