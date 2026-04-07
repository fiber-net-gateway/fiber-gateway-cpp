#ifndef FIBER_HTTP_HTTP1_CONNECTION_GROUP_HINT_TABLE_H
#define FIBER_HTTP_HTTP1_CONNECTION_GROUP_HINT_TABLE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http1ConnectionGroupKey.h"

namespace fiber::http {

class Http1ConnectionGroupHintTable : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr std::uint32_t kWays = 4;
    static constexpr std::uint32_t kSetCount = 64;
    static constexpr std::uint32_t kSlotCount = kWays * kSetCount;

    struct ProbeResult {
        std::uint8_t approx_count = 0;

        [[nodiscard]] bool may_have() const noexcept { return approx_count != 0; }
    };

    Http1ConnectionGroupHintTable() noexcept = default;

    void clear() noexcept;
    void note_idle_add(const Http1ConnectionGroupKey &key) noexcept;
    void note_idle_remove(const Http1ConnectionGroupKey &key) noexcept;
    [[nodiscard]] ProbeResult probe(const Http1ConnectionGroupKey &key) const noexcept;

private:
    struct Slot {
        std::atomic<std::uint64_t> word{0};
    };

    struct alignas(64) Set {
        std::array<Slot, kWays> slots{};
    };

    [[nodiscard]] static std::uint32_t set_index(std::uint64_t hash) noexcept;
    [[nodiscard]] static std::uint32_t fingerprint(std::uint64_t hash) noexcept;
    [[nodiscard]] static std::uint64_t pack(std::uint32_t fp, std::uint8_t count, std::uint32_t generation) noexcept;
    [[nodiscard]] static std::uint32_t unpack_fp(std::uint64_t word) noexcept;
    [[nodiscard]] static std::uint8_t unpack_count(std::uint64_t word) noexcept;
    [[nodiscard]] static std::uint32_t unpack_generation(std::uint64_t word) noexcept;
    [[nodiscard]] std::uint32_t next_generation() noexcept;

    std::array<Set, kSetCount> sets_{};
    std::uint32_t next_generation_ = 1;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP1_CONNECTION_GROUP_HINT_TABLE_H
