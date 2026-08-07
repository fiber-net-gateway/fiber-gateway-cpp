#include "RateLimitShardRing.h"

#include "RateLimitHash.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <utility>

#include <fiber/net/IpAddress.h>

namespace fiber::ai_server {
namespace {

std::int64_t java_long(std::uint64_t value) noexcept { return std::bit_cast<std::int64_t>(value); }

bool valid_node(const RateLimitNode &node) noexcept {
    net::IpAddress address;
    return !node.node_id.empty() && !node.host.empty() && node.port != 0 && node.weight != 0 &&
           net::IpAddress::parse(node.host, address) && address.is_v4() && !address.is_unspecified() &&
           !address.is_multicast();
}

} // namespace

RateLimitShardRing::RateLimitShardRing() noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    current_.store(std::make_shared<const RateLimitRingSnapshot>(), std::memory_order_release);
#else
    std::atomic_store_explicit(&current_, std::make_shared<const RateLimitRingSnapshot>(), std::memory_order_release);
#endif
}

std::expected<void, RateLimitRingError> RateLimitShardRing::update(std::uint64_t version,
                                                                   std::vector<RateLimitNode> nodes) {
    std::sort(nodes.begin(), nodes.end(),
              [](const RateLimitNode &left, const RateLimitNode &right) { return left.node_id < right.node_id; });

    std::size_t virtual_node_count = 0;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!valid_node(nodes[i])) {
            return std::unexpected(RateLimitRingError{
                    .code = RateLimitRingErrorCode::InvalidNode,
                    .node_id = nodes[i].node_id,
            });
        }
        if (i > 0 && nodes[i - 1].node_id == nodes[i].node_id) {
            return std::unexpected(RateLimitRingError{
                    .code = RateLimitRingErrorCode::DuplicateNode,
                    .node_id = nodes[i].node_id,
            });
        }
        const std::size_t count = kVirtualNodesPerWeight * nodes[i].weight;
        if (virtual_node_count > std::numeric_limits<std::size_t>::max() - count) {
            return std::unexpected(RateLimitRingError{
                    .code = RateLimitRingErrorCode::TooManyVirtualNodes,
                    .node_id = nodes[i].node_id,
            });
        }
        virtual_node_count += count;
    }

    auto next = std::make_shared<RateLimitRingSnapshot>();
    next->version = version;
    next->nodes = std::move(nodes);
    next->entries.reserve(virtual_node_count);
    for (std::size_t node_index = 0; node_index < next->nodes.size(); ++node_index) {
        const RateLimitNode &node = next->nodes[node_index];
        const std::size_t count = kVirtualNodesPerWeight * node.weight;
        for (std::size_t i = 0; i < count; ++i) {
            std::string virtual_node_key;
            virtual_node_key.reserve(node.node_id.size() + 24);
            virtual_node_key.append(node.node_id);
            virtual_node_key.push_back('#');
            virtual_node_key.append(std::to_string(i));
            next->entries.push_back(RateLimitRingSnapshot::Entry{
                    .hash = java_long(rate_limit_hash64(virtual_node_key)),
                    .node_index = node_index,
            });
        }
    }
    std::sort(next->entries.begin(), next->entries.end(),
              [&next](const RateLimitRingSnapshot::Entry &left, const RateLimitRingSnapshot::Entry &right) {
                  if (left.hash != right.hash) {
                      return left.hash < right.hash;
                  }
                  return next->nodes[left.node_index].node_id < next->nodes[right.node_index].node_id;
              });
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    current_.store(std::move(next), std::memory_order_release);
#else
    std::shared_ptr<const RateLimitRingSnapshot> published = std::move(next);
    std::atomic_store_explicit(&current_, std::move(published), std::memory_order_release);
#endif
    return {};
}

std::shared_ptr<const RateLimitRingSnapshot> RateLimitShardRing::snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return current_.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&current_, std::memory_order_acquire);
#endif
}

std::optional<RateLimitNode> RateLimitShardRing::locate(std::string_view user_id, std::string_view model_name) const {
    const auto current = snapshot();
    if (!current || current->entries.empty()) {
        return std::nullopt;
    }
    const std::int64_t key_hash = java_long(rate_limit_key_hash64(user_id, model_name));
    auto found = std::lower_bound(
            current->entries.begin(), current->entries.end(), key_hash,
            [](const RateLimitRingSnapshot::Entry &entry, std::int64_t hash) { return entry.hash < hash; });
    if (found == current->entries.end()) {
        found = current->entries.begin();
    }
    return current->nodes[found->node_index];
}

std::optional<std::string> java_self_service_node_id(std::string_view ipv4, std::uint16_t port) {
    net::IpAddress address;
    if (port == 0 || !net::IpAddress::parse(ipv4, address) || !address.is_v4() || address.is_unspecified() ||
        address.is_multicast()) {
        return std::nullopt;
    }
    const std::array<std::uint8_t, 4> bytes = address.v4_bytes();
    const std::array<std::uint8_t, 6> data = {
            bytes[3],
            bytes[2],
            bytes[1],
            bytes[0],
            static_cast<std::uint8_t>(port & 0xffU),
            static_cast<std::uint8_t>((port >> 8U) & 0xffU),
    };
    constexpr std::array<char, 16> kHex = {
            '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string output;
    output.resize(data.size() * 2);
    for (std::size_t i = 0; i < data.size(); ++i) {
        output[i * 2] = kHex[data[i] >> 4U];
        output[i * 2 + 1] = kHex[data[i] & 0x0fU];
    }
    return output;
}

} // namespace fiber::ai_server
