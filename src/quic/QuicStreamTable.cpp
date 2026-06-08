#include "QuicStreamTable.h"

#include <limits>
#include <new>

#include "../common/Assert.h"

namespace fiber::quic {

namespace {

constexpr std::size_t kMinBucketCount = 8;

} // namespace

bool QuicStreamTable::init(std::size_t initial_stream_capacity) noexcept {
    clear();
    if (initial_stream_capacity == 0) {
        return true;
    }
    return rehash(target_bucket_count(initial_stream_capacity));
}

void QuicStreamTable::clear() noexcept {
    buckets_.reset();
    bucket_count_ = 0;
    size_ = 0;
}

QuicStream *QuicStreamTable::find(std::uint64_t stream_id) noexcept {
    const std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }
    return buckets_[slot].stream;
}

const QuicStream *QuicStreamTable::find(std::uint64_t stream_id) const noexcept {
    const std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }
    return buckets_[slot].stream;
}

bool QuicStreamTable::insert(QuicStream &stream) noexcept {
    const std::uint64_t stream_id = stream.stream_id();
    if (find_slot(stream_id) != bucket_count_) {
        return false;
    }
    if (!ensure_capacity_for_insert()) {
        return false;
    }

    const std::size_t slot = find_insert_slot(buckets_.get(), bucket_count_, stream_id);
    FIBER_ASSERT(slot < bucket_count_);
    buckets_[slot] = Bucket{
            .stream_id = stream_id,
            .stream = &stream,
    };
    ++size_;
    return true;
}

QuicStream *QuicStreamTable::erase(std::uint64_t stream_id) noexcept {
    const std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }

    QuicStream *stream = buckets_[slot].stream;
    erase_at(slot);
    return stream;
}

std::size_t QuicStreamTable::next_pow2(std::size_t value) noexcept {
    if (value <= 1) {
        return 1;
    }

    std::size_t out = 1;
    while (out < value) {
        out <<= 1U;
    }
    return out;
}

std::size_t QuicStreamTable::target_bucket_count(std::size_t min_stream_capacity) noexcept {
    if (min_stream_capacity == 0) {
        return 0;
    }
    if (min_stream_capacity > (std::numeric_limits<std::size_t>::max() / 2U)) {
        return 0;
    }
    const std::size_t target = next_pow2(min_stream_capacity * 2U);
    return target < kMinBucketCount ? kMinBucketCount : target;
}

std::size_t QuicStreamTable::hash_stream_id(std::uint64_t stream_id) noexcept {
    stream_id ^= stream_id >> 30U;
    stream_id *= 0xbf58476d1ce4e5b9ULL;
    stream_id ^= stream_id >> 27U;
    stream_id *= 0x94d049bb133111ebULL;
    stream_id ^= stream_id >> 31U;
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        stream_id ^= stream_id >> 32U;
    }
    return static_cast<std::size_t>(stream_id);
}

std::size_t QuicStreamTable::mask() const noexcept { return bucket_count_ - 1U; }

std::size_t QuicStreamTable::probe_distance(std::size_t from, std::size_t to) const noexcept {
    if (to >= from) {
        return to - from;
    }
    return bucket_count_ - from + to;
}

std::size_t QuicStreamTable::find_slot(std::uint64_t stream_id) const noexcept {
    if (!buckets_ || bucket_count_ == 0) {
        return bucket_count_;
    }

    std::size_t slot = hash_stream_id(stream_id) & mask();
    for (std::size_t probed = 0; probed < bucket_count_; ++probed) {
        const Bucket &bucket = buckets_[slot];
        if (!bucket.stream) {
            return bucket_count_;
        }
        if (bucket.stream_id == stream_id) {
            return slot;
        }
        slot = (slot + 1U) & mask();
    }
    return bucket_count_;
}

bool QuicStreamTable::should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept {
    return probe_distance(home, hole) < probe_distance(home, current);
}

bool QuicStreamTable::ensure_capacity_for_insert() noexcept {
    if (bucket_count_ == 0) {
        return rehash(kMinBucketCount);
    }
    if ((size_ + 1U) * 2U <= bucket_count_) {
        return true;
    }
    if (bucket_count_ > (std::numeric_limits<std::size_t>::max() / 2U)) {
        return false;
    }
    return rehash(bucket_count_ * 2U);
}

bool QuicStreamTable::rehash(std::size_t new_bucket_count) noexcept {
    if (new_bucket_count == 0) {
        clear();
        return true;
    }
    if ((new_bucket_count & (new_bucket_count - 1U)) != 0) {
        new_bucket_count = next_pow2(new_bucket_count);
    }
    if (new_bucket_count < kMinBucketCount) {
        new_bucket_count = kMinBucketCount;
    }
    if (size_ > new_bucket_count / 2U) {
        return false;
    }

    std::unique_ptr<Bucket[]> new_buckets(new (std::nothrow) Bucket[new_bucket_count]{});
    if (!new_buckets) {
        return false;
    }

    for (std::size_t i = 0; i < bucket_count_; ++i) {
        const Bucket &old_bucket = buckets_[i];
        if (!old_bucket.stream) {
            continue;
        }
        const std::size_t slot = find_insert_slot(new_buckets.get(), new_bucket_count, old_bucket.stream_id);
        FIBER_ASSERT(slot < new_bucket_count);
        new_buckets[slot] = old_bucket;
    }

    buckets_ = std::move(new_buckets);
    bucket_count_ = new_bucket_count;
    return true;
}

std::size_t QuicStreamTable::find_insert_slot(const Bucket *buckets, std::size_t bucket_count,
                                              std::uint64_t stream_id) noexcept {
    FIBER_ASSERT(buckets != nullptr);
    FIBER_ASSERT(bucket_count > 0);
    const std::size_t mask = bucket_count - 1U;
    std::size_t slot = hash_stream_id(stream_id) & mask;
    for (std::size_t probed = 0; probed < bucket_count; ++probed) {
        if (!buckets[slot].stream) {
            return slot;
        }
        slot = (slot + 1U) & mask;
    }
    return bucket_count;
}

void QuicStreamTable::erase_at(std::size_t index) noexcept {
    std::size_t hole = index;
    std::size_t current = (hole + 1U) & mask();

    while (buckets_[current].stream) {
        const std::size_t home = hash_stream_id(buckets_[current].stream_id) & mask();
        if (should_shift_bucket(hole, current, home)) {
            buckets_[hole] = buckets_[current];
            hole = current;
        }
        current = (current + 1U) & mask();
    }

    buckets_[hole] = Bucket{};
    --size_;
}

} // namespace fiber::quic
