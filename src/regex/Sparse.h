#ifndef FIBER_REGEX_SPARSE_H
#define FIBER_REGEX_SPARSE_H

#include <bit>
#include <cstdint>
#include <vector>

#include <fiber/common/Assert.h>

namespace fiber::regex::detail {

// Briggs & Torczon sparse containers: constant-time membership and clear, with
// iteration in insertion order. The matching engines depend on that order, as it
// encodes thread priority.
template<typename Entry>
class SparseStorage {
public:
    void ensure_capacity(std::uint32_t capacity) {
        if (capacity > sparse_.size()) {
            sparse_.resize(capacity);
            dense_.resize(capacity);
        }
    }

    void clear() noexcept { size_ = 0; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

protected:
    std::vector<std::uint32_t> sparse_{};
    std::vector<Entry> dense_{};
    std::uint32_t size_ = 0;
};

class SparseSet : public SparseStorage<std::uint32_t> {
public:
    [[nodiscard]] bool contains(std::uint32_t index) const noexcept {
        const std::uint32_t slot = sparse_[index];
        return slot < size_ && dense_[slot] == index;
    }

    void insert(std::uint32_t index) noexcept {
        if (!contains(index)) {
            insert_new(index);
        }
    }

    void insert_new(std::uint32_t index) noexcept {
        sparse_[index] = size_;
        dense_[size_++] = index;
    }

    [[nodiscard]] std::uint32_t at(std::uint32_t position) const noexcept { return dense_[position]; }
};

template<typename Value>
struct SparseEntry {
    std::uint32_t index = 0;
    Value value{};
};

template<typename Value>
class SparseArray : public SparseStorage<SparseEntry<Value>> {
public:
    [[nodiscard]] bool has_index(std::uint32_t index) const noexcept {
        const std::uint32_t slot = this->sparse_[index];
        return slot < this->size_ && this->dense_[slot].index == index;
    }

    void set_new(std::uint32_t index, Value value) noexcept {
        this->sparse_[index] = this->size_;
        this->dense_[this->size_++] = SparseEntry<Value>{index, value};
    }

    [[nodiscard]] Value &get_existing(std::uint32_t index) noexcept { return this->dense_[this->sparse_[index]].value; }

    [[nodiscard]] SparseEntry<Value> &entry_at(std::uint32_t position) noexcept { return this->dense_[position]; }
};

class Bitmap256 {
public:
    void clear() noexcept {
        for (std::uint64_t &word: words_) {
            word = 0;
        }
    }

    [[nodiscard]] bool test(std::uint32_t bit) const noexcept { return (words_[bit / 64U] >> (bit % 64U) & 1U) != 0; }
    void set(std::uint32_t bit) noexcept { words_[bit / 64U] |= std::uint64_t{1} << (bit % 64U); }

    // Index of the first set bit at or after `bit`, which must exist.
    [[nodiscard]] std::uint32_t find_next_set_bit(std::uint32_t bit) const noexcept {
        std::uint32_t word_index = bit / 64U;
        std::uint64_t word = words_[word_index] & (~std::uint64_t{0} << (bit % 64U));
        while (word == 0) {
            ++word_index;
            FIBER_ASSERT(word_index < 4U);
            word = words_[word_index];
        }
        return (word_index * 64U) + static_cast<std::uint32_t>(std::countr_zero(word));
    }

private:
    std::uint64_t words_[4]{};
};

} // namespace fiber::regex::detail

#endif // FIBER_REGEX_SPARSE_H
