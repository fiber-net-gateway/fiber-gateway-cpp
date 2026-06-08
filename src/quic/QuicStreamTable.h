#ifndef FIBER_QUIC_QUIC_STREAM_TABLE_H
#define FIBER_QUIC_QUIC_STREAM_TABLE_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "QuicStream.h"

namespace fiber::quic {

class QuicStreamTable : public common::NonCopyable, public common::NonMovable {
public:
    QuicStreamTable() noexcept = default;
    ~QuicStreamTable();

    [[nodiscard]] bool init(std::size_t initial_stream_capacity = 0) noexcept;
    void clear() noexcept;

    [[nodiscard]] QuicStream *find(std::uint64_t stream_id) noexcept;
    [[nodiscard]] const QuicStream *find(std::uint64_t stream_id) const noexcept;

    [[nodiscard]] bool insert(QuicStream::Lease &&stream) noexcept;
    [[nodiscard]] QuicStream::Lease erase(std::uint64_t stream_id) noexcept;

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
        std::uint64_t stream_id = 0;
        QuicStream *stream = nullptr;
    };

    [[nodiscard]] static std::size_t next_pow2(std::size_t value) noexcept;
    [[nodiscard]] static std::size_t target_bucket_count(std::size_t min_stream_capacity) noexcept;
    [[nodiscard]] static std::size_t hash_stream_id(std::uint64_t stream_id) noexcept;
    [[nodiscard]] std::size_t mask() const noexcept;
    [[nodiscard]] std::size_t probe_distance(std::size_t from, std::size_t to) const noexcept;
    [[nodiscard]] std::size_t find_slot(std::uint64_t stream_id) const noexcept;
    [[nodiscard]] bool should_shift_bucket(std::size_t hole, std::size_t current, std::size_t home) const noexcept;
    [[nodiscard]] bool ensure_capacity_for_insert() noexcept;
    [[nodiscard]] bool rehash(std::size_t new_bucket_count) noexcept;
    [[nodiscard]] static std::size_t find_insert_slot(const Bucket *buckets, std::size_t bucket_count,
                                                      std::uint64_t stream_id) noexcept;
    void erase_at(std::size_t index) noexcept;

    std::unique_ptr<Bucket[]> buckets_{};
    std::size_t bucket_count_ = 0;
    std::size_t size_ = 0;
};

} // namespace fiber::quic

#endif // FIBER_QUIC_QUIC_STREAM_TABLE_H
