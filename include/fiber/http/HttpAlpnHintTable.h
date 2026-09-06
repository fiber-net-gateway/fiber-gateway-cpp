#ifndef FIBER_HTTP_HTTP_ALPN_HINT_TABLE_H
#define FIBER_HTTP_HTTP_ALPN_HINT_TABLE_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "HttpClientDialer.h"
#include "HttpConnectionGroupKey.h"

namespace fiber::http {

// Remembers which HTTP version an origin negotiated, so a later request to the same origin can go
// straight to the right pool instead of paying for another TLS handshake to find out. A hint is
// only ever an optimisation: a stale or evicted entry costs one negotiating dial, never
// correctness, which is why entries are direct-mapped and simply overwrite each other on
// collision rather than chaining.
//
// Single-threaded, like the per-loop pool cores it sits next to.
class HttpAlpnHintTable : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr std::size_t kSlotCount = 256;
    static constexpr std::chrono::milliseconds kDefaultTtl{600000};

    HttpAlpnHintTable() noexcept = default;
    explicit HttpAlpnHintTable(std::chrono::milliseconds ttl) noexcept : ttl_(ttl) {}

    void note(const HttpConnectionGroupKey &key, HttpProtocol protocol,
              std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::optional<HttpProtocol> lookup(const HttpConnectionGroupKey &key,
                                                     std::chrono::steady_clock::time_point now) const noexcept;
    // Drops the hint for one origin. Used when a connection to it turned out not to speak the
    // remembered protocol after all.
    void forget(const HttpConnectionGroupKey &key) noexcept;
    void clear() noexcept;

    [[nodiscard]] std::chrono::milliseconds ttl() const noexcept { return ttl_; }

private:
    struct Slot {
        std::uint64_t hash = 0;
        std::chrono::steady_clock::time_point expire_at{};
        HttpProtocol protocol = HttpProtocol::Http1;
        bool occupied = false;
    };

    [[nodiscard]] static std::size_t slot_index(std::uint64_t hash) noexcept {
        return static_cast<std::size_t>(hash) & (kSlotCount - 1);
    }

    std::array<Slot, kSlotCount> slots_{};
    std::chrono::milliseconds ttl_{kDefaultTtl};
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_ALPN_HINT_TABLE_H
