#ifndef FIBER_HTTP_HTTP_CONNECTION_BUCKET_INDEX_H
#define FIBER_HTTP_HTTP_CONNECTION_BUCKET_INDEX_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpConnectionGroupKey.h"
#include "HttpConnectionPoolBucketBase.h"

namespace fiber::http {

class HttpConnectionBucketIndex : public common::NonCopyable, public common::NonMovable {
public:
    struct EntryRef {
        std::uint32_t slot_index = HttpConnectionPoolBucketBase::kInvalidSlotIndex;
        HttpConnectionPoolBucketBase *bucket = nullptr;

        [[nodiscard]] explicit operator bool() const noexcept { return bucket != nullptr; }
    };

    HttpConnectionBucketIndex() noexcept = default;
    ~HttpConnectionBucketIndex();

    [[nodiscard]] bool init(std::size_t initial_group_capacity = 0) noexcept;
    void clear() noexcept;

    [[nodiscard]] EntryRef find(const HttpConnectionGroupKey &key) noexcept;
    [[nodiscard]] common::IoErr insert(const HttpConnectionGroupKey &key,
                                       HttpConnectionPoolBucketBase &bucket) noexcept;
    void erase(std::uint32_t slot_index) noexcept;

    [[nodiscard]] const HttpConnectionGroupKey *key_at(std::uint32_t slot_index) const noexcept;
    [[nodiscard]] HttpConnectionPoolBucketBase *bucket_at(std::uint32_t slot_index) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t slot_capacity() const noexcept { return slot_capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

private:
    struct Slot {
        std::uint64_t hash = 0;
        HttpConnectionPoolBucketBase *bucket = nullptr;
        alignas(HttpConnectionGroupKey) std::byte key_storage[sizeof(HttpConnectionGroupKey)];

        [[nodiscard]] bool occupied() const noexcept { return bucket != nullptr; }
        [[nodiscard]] HttpConnectionGroupKey *key() noexcept;
        [[nodiscard]] const HttpConnectionGroupKey *key() const noexcept;
    };

    [[nodiscard]] static std::size_t next_pow2(std::size_t value) noexcept;
    [[nodiscard]] static std::size_t target_slot_capacity(std::size_t min_group_capacity) noexcept;
    [[nodiscard]] std::size_t mask() const noexcept;
    [[nodiscard]] std::size_t probe_distance(std::size_t from, std::size_t to) const noexcept;
    [[nodiscard]] std::size_t find_slot(const HttpConnectionGroupKey &key) const noexcept;
    [[nodiscard]] bool should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept;
    [[nodiscard]] bool ensure_capacity_for_insert() noexcept;
    [[nodiscard]] bool rehash(std::size_t new_slot_capacity) noexcept;
    [[nodiscard]] std::size_t find_insert_slot(const Slot *slots, std::size_t slot_capacity,
                                               const HttpConnectionGroupKey &key) const noexcept;
    static void destroy_slot(Slot &slot) noexcept;
    static void move_slot(Slot &dst, std::size_t dst_index, Slot &src) noexcept;
    void erase_at(std::size_t slot_index) noexcept;

    std::unique_ptr<Slot[]> slots_{};
    std::size_t slot_capacity_ = 0;
    std::size_t size_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_CONNECTION_BUCKET_INDEX_H
