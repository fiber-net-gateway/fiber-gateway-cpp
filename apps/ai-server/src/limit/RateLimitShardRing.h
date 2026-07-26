#ifndef FIBER_AI_SERVER_RATE_LIMIT_SHARD_RING_H
#define FIBER_AI_SERVER_RATE_LIMIT_SHARD_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <common/NonCopyable.h>
#include <common/NonMovable.h>

namespace fiber::ai_server {

struct RateLimitNode {
    std::string node_id;
    std::string host;
    std::uint16_t port = 0;
    std::uint16_t weight = 1;
    bool local = false;

    friend bool operator==(const RateLimitNode &, const RateLimitNode &) = default;
};

enum class RateLimitRingErrorCode : std::uint8_t {
    InvalidNode,
    DuplicateNode,
    TooManyVirtualNodes,
};

struct RateLimitRingError {
    RateLimitRingErrorCode code = RateLimitRingErrorCode::InvalidNode;
    std::string node_id;
};

struct RateLimitRingSnapshot {
    struct Entry {
        std::int64_t hash = 0;
        std::size_t node_index = 0;
    };

    std::uint64_t version = 0;
    std::vector<RateLimitNode> nodes;
    std::vector<Entry> entries;
};

class RateLimitShardRing final : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr std::size_t kVirtualNodesPerWeight = 200;

    RateLimitShardRing() noexcept;

    [[nodiscard]] std::expected<void, RateLimitRingError> update(std::uint64_t version,
                                                                 std::vector<RateLimitNode> nodes);

    [[nodiscard]] std::shared_ptr<const RateLimitRingSnapshot> snapshot() const noexcept;
    [[nodiscard]] std::optional<RateLimitNode> locate(std::string_view user_id, std::string_view model_name) const;

private:
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const RateLimitRingSnapshot>> current_;
#else
    std::shared_ptr<const RateLimitRingSnapshot> current_;
#endif
};

[[nodiscard]] std::optional<std::string> java_self_service_node_id(std::string_view ipv4, std::uint16_t port);

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_RATE_LIMIT_SHARD_RING_H
