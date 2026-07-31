#ifndef FIBER_ACCESS_SERVER_HOST_MATCHER_H
#define FIBER_ACCESS_SERVER_HOST_MATCHER_H

#include "../config/AccessConfigError.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fiber::access_server {

struct HostPattern {
    std::string_view pattern;
    std::uint32_t handler = 0;
};

class HostMatcher {
public:
    HostMatcher();

    [[nodiscard]] static std::expected<HostMatcher, AccessConfigError> build(std::span<const HostPattern> patterns);

    // The request Host validation and matching behavior follows
    // HostMatchNameFetcher/WildHostNode, including its exact-branch fallback
    // rule. The returned handler is an index owned by the enclosing snapshot.
    [[nodiscard]] std::optional<std::uint32_t> match(std::string_view host) const noexcept;

    [[nodiscard]] bool empty() const noexcept;

private:
    static constexpr std::uint32_t kNoHandler = UINT32_MAX;

    struct Child {
        std::string label;
        std::uint32_t node = 0;
    };

    struct Node {
        std::vector<Child> children;
        std::uint32_t exact_handler = kNoHandler;
        std::uint32_t wildcard_handler = kNoHandler;
    };

    [[nodiscard]] std::uint32_t find_child(std::uint32_t node, std::string_view label) const noexcept;
    [[nodiscard]] std::uint32_t add_or_find_child(std::uint32_t node, std::string_view label);
    [[nodiscard]] std::uint32_t match_node(std::uint32_t node, std::string_view host, std::size_t end) const noexcept;

    std::vector<Node> nodes_;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_HOST_MATCHER_H
