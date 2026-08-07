#include <fiber/http/Http1ConnectionGroupHintTable.h>

#include <algorithm>

namespace fiber::http {

namespace {

constexpr std::uint64_t kCountShift = 32;
constexpr std::uint64_t kGenerationShift = 40;
constexpr std::uint64_t kFingerprintMask = 0xffffffffULL;
constexpr std::uint64_t kCountMask = 0xffULL;
constexpr std::uint64_t kGenerationMask = 0xffffffULL;

} // namespace

void Http1ConnectionGroupHintTable::clear() noexcept {
    for (auto &set: sets_) {
        for (auto &slot: set.slots) {
            slot.word.store(0, std::memory_order_release);
        }
    }
    next_generation_ = 1;
}

void Http1ConnectionGroupHintTable::note_idle_add(const Http1ConnectionGroupKey &key) noexcept {
    const std::uint64_t hash = key.hash();
    const std::uint32_t fp = fingerprint(hash);
    Set &set = sets_[set_index(hash)];

    std::uint32_t empty_index = kWays;
    std::uint32_t replace_index = 0;
    std::uint32_t oldest_generation = 0xffffffffU;
    for (std::uint32_t i = 0; i < kWays; ++i) {
        const std::uint64_t word = set.slots[i].word.load(std::memory_order_relaxed);
        const std::uint8_t count = unpack_count(word);
        if (count == 0) {
            if (empty_index == kWays) {
                empty_index = i;
            }
            continue;
        }
        if (unpack_fp(word) == fp) {
            const std::uint8_t next_count = count == 0xffU ? count : static_cast<std::uint8_t>(count + 1U);
            set.slots[i].word.store(pack(fp, next_count, next_generation()), std::memory_order_release);
            return;
        }
        const std::uint32_t generation = unpack_generation(word);
        if (generation < oldest_generation) {
            oldest_generation = generation;
            replace_index = i;
        }
    }

    const std::uint32_t target_index = empty_index != kWays ? empty_index : replace_index;
    set.slots[target_index].word.store(pack(fp, 1, next_generation()), std::memory_order_release);
}

void Http1ConnectionGroupHintTable::note_idle_remove(const Http1ConnectionGroupKey &key) noexcept {
    const std::uint64_t hash = key.hash();
    const std::uint32_t fp = fingerprint(hash);
    Set &set = sets_[set_index(hash)];

    for (std::uint32_t i = 0; i < kWays; ++i) {
        const std::uint64_t word = set.slots[i].word.load(std::memory_order_relaxed);
        const std::uint8_t count = unpack_count(word);
        if (count == 0 || unpack_fp(word) != fp) {
            continue;
        }
        const std::uint8_t next_count = static_cast<std::uint8_t>(count - 1U);
        set.slots[i].word.store(pack(fp, next_count, next_generation()), std::memory_order_release);
        return;
    }
}

Http1ConnectionGroupHintTable::ProbeResult
Http1ConnectionGroupHintTable::probe(const Http1ConnectionGroupKey &key) const noexcept {
    const std::uint64_t hash = key.hash();
    const std::uint32_t fp = fingerprint(hash);
    const Set &set = sets_[set_index(hash)];

    std::uint8_t best_count = 0;
    for (const auto &slot: set.slots) {
        const std::uint64_t word = slot.word.load(std::memory_order_acquire);
        const std::uint8_t count = unpack_count(word);
        if (count == 0 || unpack_fp(word) != fp) {
            continue;
        }
        best_count = std::max(best_count, count);
    }
    return ProbeResult{.approx_count = best_count};
}

std::uint32_t Http1ConnectionGroupHintTable::set_index(std::uint64_t hash) noexcept {
    return static_cast<std::uint32_t>((hash >> 32U) & (kSetCount - 1U));
}

std::uint32_t Http1ConnectionGroupHintTable::fingerprint(std::uint64_t hash) noexcept {
    return static_cast<std::uint32_t>(hash ^ (hash >> 32U));
}

std::uint64_t Http1ConnectionGroupHintTable::pack(std::uint32_t fp, std::uint8_t count,
                                                  std::uint32_t generation) noexcept {
    return static_cast<std::uint64_t>(fp) | (static_cast<std::uint64_t>(count) << kCountShift) |
           ((static_cast<std::uint64_t>(generation) & kGenerationMask) << kGenerationShift);
}

std::uint32_t Http1ConnectionGroupHintTable::unpack_fp(std::uint64_t word) noexcept {
    return static_cast<std::uint32_t>(word & kFingerprintMask);
}

std::uint8_t Http1ConnectionGroupHintTable::unpack_count(std::uint64_t word) noexcept {
    return static_cast<std::uint8_t>((word >> kCountShift) & kCountMask);
}

std::uint32_t Http1ConnectionGroupHintTable::unpack_generation(std::uint64_t word) noexcept {
    return static_cast<std::uint32_t>((word >> kGenerationShift) & kGenerationMask);
}

std::uint32_t Http1ConnectionGroupHintTable::next_generation() noexcept {
    const std::uint32_t generation = next_generation_;
    ++next_generation_;
    if (next_generation_ == 0) {
        next_generation_ = 1;
    }
    return generation;
}

} // namespace fiber::http
