#include "Http1ConnectionBucketIndex.h"

#include <limits>
#include <memory>
#include <new>

#include "../common/Assert.h"

namespace fiber::http {

namespace {

constexpr std::size_t kMinSlotCapacity = 8;

} // namespace

Http1ConnectionBucketIndex::~Http1ConnectionBucketIndex() { clear(); }

bool Http1ConnectionBucketIndex::init(std::size_t initial_group_capacity) noexcept {
    clear();
    if (initial_group_capacity == 0) {
        return true;
    }
    return rehash(target_slot_capacity(initial_group_capacity));
}

void Http1ConnectionBucketIndex::clear() noexcept {
    if (slots_) {
        for (std::size_t i = 0; i < slot_capacity_; ++i) {
            Slot &slot = slots_[i];
            if (!slot.occupied()) {
                continue;
            }
            if (slot.bucket) {
                slot.bucket->slot_index_ = Http1ConnectionPoolGroupBucket::kInvalidSlotIndex;
            }
            destroy_slot(slot);
        }
    }
    slots_.reset();
    slot_capacity_ = 0;
    size_ = 0;
}

Http1ConnectionBucketIndex::EntryRef Http1ConnectionBucketIndex::find(const Http1ConnectionGroupKey &key) noexcept {
    const std::size_t slot_index = find_slot(key);
    if (slot_index == slot_capacity_) {
        return {};
    }
    return EntryRef{
            .slot_index = static_cast<std::uint32_t>(slot_index),
            .bucket = slots_[slot_index].bucket,
    };
}

common::IoErr Http1ConnectionBucketIndex::insert(const Http1ConnectionGroupKey &key,
                                                 Http1ConnectionPoolGroupBucket &bucket) noexcept {
    if (bucket.slot_index_ != Http1ConnectionPoolGroupBucket::kInvalidSlotIndex) {
        return common::IoErr::Busy;
    }
    if (find_slot(key) != slot_capacity_) {
        return common::IoErr::Already;
    }
    if (!ensure_capacity_for_insert()) {
        return common::IoErr::NoMem;
    }

    const std::size_t slot_index = find_insert_slot(slots_.get(), slot_capacity_, key);
    FIBER_ASSERT(slot_index < slot_capacity_);
    Slot &slot = slots_[slot_index];
    FIBER_ASSERT(!slot.occupied());
    std::construct_at(slot.key(), key);
    slot.hash = key.hash();
    slot.bucket = &bucket;
    bucket.slot_index_ = static_cast<std::uint32_t>(slot_index);
    ++size_;
    return common::IoErr::None;
}

void Http1ConnectionBucketIndex::erase(std::uint32_t slot_index) noexcept {
    if (!slots_ || slot_index >= slot_capacity_ || !slots_[slot_index].occupied()) {
        return;
    }
    erase_at(slot_index);
}

const Http1ConnectionGroupKey *Http1ConnectionBucketIndex::key_at(std::uint32_t slot_index) const noexcept {
    if (!slots_ || slot_index >= slot_capacity_ || !slots_[slot_index].occupied()) {
        return nullptr;
    }
    return slots_[slot_index].key();
}

Http1ConnectionPoolGroupBucket *Http1ConnectionBucketIndex::bucket_at(std::uint32_t slot_index) noexcept {
    if (!slots_ || slot_index >= slot_capacity_ || !slots_[slot_index].occupied()) {
        return nullptr;
    }
    return slots_[slot_index].bucket;
}

Http1ConnectionGroupKey *Http1ConnectionBucketIndex::Slot::key() noexcept {
    return std::launder(reinterpret_cast<Http1ConnectionGroupKey *>(key_storage));
}

const Http1ConnectionGroupKey *Http1ConnectionBucketIndex::Slot::key() const noexcept {
    return std::launder(reinterpret_cast<const Http1ConnectionGroupKey *>(key_storage));
}

std::size_t Http1ConnectionBucketIndex::next_pow2(std::size_t value) noexcept {
    if (value <= 1) {
        return 1;
    }

    std::size_t out = 1;
    while (out < value) {
        out <<= 1U;
    }
    return out;
}

std::size_t Http1ConnectionBucketIndex::target_slot_capacity(std::size_t min_group_capacity) noexcept {
    if (min_group_capacity == 0) {
        return 0;
    }
    if (min_group_capacity > (std::numeric_limits<std::size_t>::max() / 2U)) {
        return 0;
    }
    const std::size_t target = next_pow2(min_group_capacity * 2U);
    return target < kMinSlotCapacity ? kMinSlotCapacity : target;
}

std::size_t Http1ConnectionBucketIndex::mask() const noexcept { return slot_capacity_ - 1U; }

std::size_t Http1ConnectionBucketIndex::probe_distance(std::size_t from, std::size_t to) const noexcept {
    if (to >= from) {
        return to - from;
    }
    return slot_capacity_ - from + to;
}

std::size_t Http1ConnectionBucketIndex::find_slot(const Http1ConnectionGroupKey &key) const noexcept {
    if (!slots_ || slot_capacity_ == 0) {
        return slot_capacity_;
    }

    std::size_t slot_index = static_cast<std::size_t>(key.hash()) & mask();
    for (std::size_t probed = 0; probed < slot_capacity_; ++probed) {
        const Slot &slot = slots_[slot_index];
        if (!slot.occupied()) {
            return slot_capacity_;
        }
        if (slot.hash == key.hash() && *slot.key() == key) {
            return slot_index;
        }
        slot_index = (slot_index + 1U) & mask();
    }
    return slot_capacity_;
}

bool Http1ConnectionBucketIndex::should_shift_bucket(std::size_t hole, std::size_t current,
                                                     std::size_t home) const noexcept {
    return probe_distance(home, hole) < probe_distance(home, current);
}

bool Http1ConnectionBucketIndex::ensure_capacity_for_insert() noexcept {
    if (slot_capacity_ == 0) {
        return rehash(kMinSlotCapacity);
    }
    if ((size_ + 1U) * 2U <= slot_capacity_) {
        return true;
    }
    if (slot_capacity_ > (std::numeric_limits<std::size_t>::max() / 2U)) {
        return false;
    }
    return rehash(slot_capacity_ * 2U);
}

bool Http1ConnectionBucketIndex::rehash(std::size_t new_slot_capacity) noexcept {
    if (new_slot_capacity == 0) {
        clear();
        return true;
    }
    if ((new_slot_capacity & (new_slot_capacity - 1U)) != 0) {
        new_slot_capacity = next_pow2(new_slot_capacity);
    }
    if (new_slot_capacity < kMinSlotCapacity) {
        new_slot_capacity = kMinSlotCapacity;
    }
    if (size_ > new_slot_capacity / 2U) {
        return false;
    }

    std::unique_ptr<Slot[]> new_slots(new (std::nothrow) Slot[new_slot_capacity]{});
    if (!new_slots) {
        return false;
    }

    for (std::size_t i = 0; i < slot_capacity_; ++i) {
        Slot &old_slot = slots_[i];
        if (!old_slot.occupied()) {
            continue;
        }

        const std::size_t new_index = find_insert_slot(new_slots.get(), new_slot_capacity, *old_slot.key());
        FIBER_ASSERT(new_index < new_slot_capacity);
        Slot &new_slot = new_slots[new_index];
        std::construct_at(new_slot.key(), *old_slot.key());
        new_slot.hash = old_slot.hash;
        new_slot.bucket = old_slot.bucket;
        if (new_slot.bucket) {
            new_slot.bucket->slot_index_ = static_cast<std::uint32_t>(new_index);
        }
        destroy_slot(old_slot);
    }

    slots_ = std::move(new_slots);
    slot_capacity_ = new_slot_capacity;
    return true;
}

std::size_t Http1ConnectionBucketIndex::find_insert_slot(const Slot *slots, std::size_t slot_capacity,
                                                         const Http1ConnectionGroupKey &key) const noexcept {
    FIBER_ASSERT(slots != nullptr);
    FIBER_ASSERT(slot_capacity > 0);
    const std::size_t mask = slot_capacity - 1U;
    std::size_t slot_index = static_cast<std::size_t>(key.hash()) & mask;
    for (std::size_t probed = 0; probed < slot_capacity; ++probed) {
        if (!slots[slot_index].occupied()) {
            return slot_index;
        }
        slot_index = (slot_index + 1U) & mask;
    }
    return slot_capacity;
}

void Http1ConnectionBucketIndex::destroy_slot(Slot &slot) noexcept {
    if (!slot.occupied()) {
        return;
    }
    std::destroy_at(slot.key());
    slot.hash = 0;
    slot.bucket = nullptr;
}

void Http1ConnectionBucketIndex::move_slot(Slot &dst, std::size_t dst_index, Slot &src) noexcept {
    FIBER_ASSERT(!dst.occupied());
    FIBER_ASSERT(src.occupied());
    std::construct_at(dst.key(), *src.key());
    dst.hash = src.hash;
    dst.bucket = src.bucket;
    if (dst.bucket) {
        dst.bucket->slot_index_ = static_cast<std::uint32_t>(dst_index);
    }
    destroy_slot(src);
}

void Http1ConnectionBucketIndex::erase_at(std::size_t slot_index) noexcept {
    Slot &removed = slots_[slot_index];
    FIBER_ASSERT(removed.occupied());
    if (removed.bucket) {
        removed.bucket->slot_index_ = Http1ConnectionPoolGroupBucket::kInvalidSlotIndex;
    }
    destroy_slot(removed);

    std::size_t hole = slot_index;
    std::size_t current = (hole + 1U) & mask();

    while (slots_[current].occupied()) {
        const std::size_t home = static_cast<std::size_t>(slots_[current].hash) & mask();
        if (should_shift_bucket(hole, current, home)) {
            move_slot(slots_[hole], hole, slots_[current]);
            hole = current;
        }
        current = (current + 1U) & mask();
    }

    --size_;
}

} // namespace fiber::http
