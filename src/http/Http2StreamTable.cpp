#include <fiber/http/Http2StreamTable.h>

#include <limits>
#include <new>

#include <fiber/common/Assert.h>

namespace fiber::http {

Http2StreamTable::~Http2StreamTable() { clear(); }

void Http2StreamTable::clear() noexcept {
    if (buckets_) {
        for (std::size_t i = 0; i < bucket_count_; ++i) {
            if (buckets_[i].stream) {
                Http2Stream::Lease::adopt(buckets_[i].stream).reset();
                buckets_[i] = Bucket{};
            }
        }
    }
    buckets_.reset();
    bucket_count_ = 0;
    size_ = 0;
}

Http2Stream *Http2StreamTable::find(std::uint32_t stream_id) noexcept {
    std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }
    return buckets_[slot].stream;
}

const Http2Stream *Http2StreamTable::find(std::uint32_t stream_id) const noexcept {
    std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return nullptr;
    }
    return buckets_[slot].stream;
}

bool Http2StreamTable::insert(Http2Stream::Lease &&lease) noexcept {
    Http2Stream *stream = lease.get();
    if (!stream) {
        return false;
    }
    if (!ensure_capacity_for_insert()) {
        return false;
    }

    std::uint32_t stream_id = stream->stream_id();
    std::size_t idx = hash_stream_id(stream_id) & mask();
    for (std::size_t probed = 0; probed < bucket_count_; ++probed) {
        Bucket &bucket = buckets_[idx];
        if (!bucket.stream) {
            bucket.stream_id = stream_id;
            bucket.stream = lease.release_raw();
            ++size_;
            return true;
        }
        if (bucket.stream_id == stream_id) {
            return false;
        }
        idx = (idx + 1) & mask();
    }

    return false;
}

// Kept below half load so the insert probe above always terminates on a hole.
bool Http2StreamTable::ensure_capacity_for_insert() noexcept {
    if (bucket_count_ == 0) {
        return rehash(kInitialBucketCount);
    }
    if ((size_ + 1) * 2 <= bucket_count_) {
        return true;
    }
    if (bucket_count_ > (std::numeric_limits<std::size_t>::max() / 2)) {
        return false;
    }
    return rehash(bucket_count_ * 2);
}

bool Http2StreamTable::rehash(std::size_t new_bucket_count) noexcept {
    FIBER_ASSERT(new_bucket_count >= kInitialBucketCount);
    FIBER_ASSERT((new_bucket_count & (new_bucket_count - 1)) == 0);

    auto *fresh = new (std::nothrow) Bucket[new_bucket_count]{};
    if (!fresh) {
        return false;
    }

    const std::size_t new_mask = new_bucket_count - 1;
    for (std::size_t i = 0; i < bucket_count_; ++i) {
        Bucket &src = buckets_[i];
        if (!src.stream) {
            continue;
        }
        std::size_t idx = hash_stream_id(src.stream_id) & new_mask;
        while (fresh[idx].stream) {
            idx = (idx + 1) & new_mask;
        }
        fresh[idx] = src;
    }

    buckets_.reset(fresh);
    bucket_count_ = new_bucket_count;
    return true;
}

void Http2StreamTable::release_buckets() noexcept {
    buckets_.reset();
    bucket_count_ = 0;
}

Http2Stream::Lease Http2StreamTable::erase(std::uint32_t stream_id) noexcept {
    std::size_t slot = find_slot(stream_id);
    if (slot == bucket_count_) {
        return {};
    }

    Http2Stream *stream = buckets_[slot].stream;
    erase_at(slot);
    return Http2Stream::Lease::adopt(stream);
}

std::size_t Http2StreamTable::hash_stream_id(std::uint32_t stream_id) noexcept {
    std::uint32_t value = stream_id * 2654435761u;
    value ^= value >> 16;
    return value;
}

std::size_t Http2StreamTable::mask() const noexcept { return bucket_count_ - 1; }

std::size_t Http2StreamTable::probe_distance(std::size_t from, std::size_t to) const noexcept {
    if (to >= from) {
        return to - from;
    }
    return bucket_count_ - from + to;
}

std::size_t Http2StreamTable::find_slot(std::uint32_t stream_id) const noexcept {
    if (!buckets_ || bucket_count_ == 0) {
        return bucket_count_;
    }

    std::size_t idx = hash_stream_id(stream_id) & mask();
    for (std::size_t probed = 0; probed < bucket_count_; ++probed) {
        const Bucket &bucket = buckets_[idx];
        if (!bucket.stream) {
            return bucket_count_;
        }
        if (bucket.stream_id == stream_id) {
            return idx;
        }
        idx = (idx + 1) & mask();
    }
    return bucket_count_;
}

bool Http2StreamTable::should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept {
    return probe_distance(home, hole) < probe_distance(home, current);
}

void Http2StreamTable::erase_at(std::size_t index) noexcept {
    std::size_t hole = index;
    std::size_t current = (hole + 1) & mask();

    while (buckets_[current].stream) {
        std::size_t home = hash_stream_id(buckets_[current].stream_id) & mask();
        if (should_shift_bucket(hole, current, home)) {
            buckets_[hole] = buckets_[current];
            hole = current;
        }
        current = (current + 1) & mask();
    }

    buckets_[hole] = Bucket{};
    --size_;
    // A table that never grew keeps its buckets: a client running one request
    // at a time would otherwise reallocate on every stream.
    if (size_ == 0 && bucket_count_ > kInitialBucketCount) {
        release_buckets();
    }
}

} // namespace fiber::http
