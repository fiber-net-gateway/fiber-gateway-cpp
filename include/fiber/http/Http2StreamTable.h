#ifndef FIBER_HTTP_HTTP2_STREAM_TABLE_H
#define FIBER_HTTP_HTTP2_STREAM_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Stream.h"

namespace fiber::http {

class Http2StreamTable : public common::NonCopyable, public common::NonMovable {
public:
    // Buckets are allocated on the first insert and doubled whenever the table
    // would pass half load. A table that has grown past this size releases its
    // buckets again once it drains; a table still at this size keeps them, so a
    // client running one request at a time never churns the allocation.
    static constexpr std::size_t kInitialBucketCount = 8;

    Http2StreamTable() noexcept = default;
    ~Http2StreamTable();

    void clear() noexcept;

    [[nodiscard]] Http2Stream *find(std::uint32_t stream_id) noexcept;
    [[nodiscard]] const Http2Stream *find(std::uint32_t stream_id) const noexcept;

    [[nodiscard]] bool insert(Http2Stream::Lease &&stream) noexcept;
    [[nodiscard]] Http2Stream::Lease erase(std::uint32_t stream_id) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t bucket_count() const noexcept { return bucket_count_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    template<typename Fn>
    void for_each(Fn &&fn) noexcept {
        for (std::size_t i = 0; i < bucket_count_; ++i) {
            if (buckets_[i].stream) {
                fn(*buckets_[i].stream);
            }
        }
    }

    template<typename Fn>
    void for_each(Fn &&fn) const noexcept {
        for (std::size_t i = 0; i < bucket_count_; ++i) {
            if (buckets_[i].stream) {
                fn(*buckets_[i].stream);
            }
        }
    }

private:
    struct Bucket {
        std::uint32_t stream_id = 0;
        Http2Stream *stream = nullptr;
    };

    [[nodiscard]] static std::size_t hash_stream_id(std::uint32_t stream_id) noexcept;
    [[nodiscard]] bool ensure_capacity_for_insert() noexcept;
    [[nodiscard]] bool rehash(std::size_t new_bucket_count) noexcept;
    void release_buckets() noexcept;
    [[nodiscard]] std::size_t mask() const noexcept;
    [[nodiscard]] std::size_t probe_distance(std::size_t from, std::size_t to) const noexcept;
    [[nodiscard]] std::size_t find_slot(std::uint32_t stream_id) const noexcept;
    [[nodiscard]] bool should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept;
    void erase_at(std::size_t index) noexcept;

    std::unique_ptr<Bucket[]> buckets_;
    std::size_t bucket_count_ = 0;
    std::size_t size_ = 0;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_STREAM_TABLE_H
