#ifndef FIBER_UTIL_ROUTE_PATH_MATCHER_H
#define FIBER_UTIL_ROUTE_PATH_MATCHER_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::util {

class RoutePatternError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

template<typename Handler>
class RoutePathMatcher {
public:
    static constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    template<typename BuilderPayload, typename RouteVarDefiner>
    class Builder;

    RoutePathMatcher() = default;

    [[nodiscard]] std::uint32_t max_path_var_count() const noexcept { return max_path_var_count_; }

    [[nodiscard]] bool empty() const noexcept { return handlers_.empty(); }

    template<typename Context>
    [[nodiscard]] bool match_path(std::string_view path, Context &context) const {
        if (nodes_.empty()) {
            return false;
        }
        return exec(path, 0, 0, context);
    }

private:
    struct Node {
        std::uint32_t name_offset = 0;
        std::uint32_t name_size = 0;
        std::uint32_t hash = 0;
        std::uint32_t id = kInvalidIndex;
        std::uint32_t static_slot_begin = 0;
        std::uint32_t static_slot_count = 0;
        std::uint32_t placeholder_begin = 0;
        std::uint32_t placeholder_count = 0;
        std::uint32_t wildcard_begin = 0;
        std::uint32_t wildcard_count = 0;
        std::uint32_t handler_begin = 0;
        std::uint32_t handler_count = 0;
    };

    template<typename BuilderPayload, typename RouteVarDefiner>
    friend class Builder;

    [[nodiscard]] std::string_view node_name_view(const Node &node) const noexcept {
        return std::string_view(text_.data() + node.name_offset, node.name_size);
    }

    [[nodiscard]] bool node_name_equals(const Node &node, const char *segment_data, std::size_t segment_size,
                                        std::uint32_t hash) const noexcept {
        if (node.hash != hash || node.name_size != segment_size) {
            return false;
        }
        const char *name = text_.data() + node.name_offset;
        for (std::size_t i = 0; i < segment_size; ++i) {
            if (name[i] != segment_data[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint32_t find_static_child(const Node &node, const char *segment_data, std::size_t segment_size,
                                                  std::uint32_t hash) const noexcept {
        if (node.static_slot_count == 0) {
            return kInvalidIndex;
        }
        std::uint32_t slot = hash & (node.static_slot_count - 1);
        while (true) {
            const std::uint32_t child_index = static_slots_[node.static_slot_begin + slot];
            if (child_index == kInvalidIndex) {
                return kInvalidIndex;
            }
            const Node &child = nodes_[child_index];
            if (node_name_equals(child, segment_data, segment_size, hash)) {
                return child_index;
            }
            slot = (slot + 1) & (node.static_slot_count - 1);
        }
    }

    template<typename Context>
    [[nodiscard]] bool match_node(std::uint32_t node_index, Context &context) const {
        const Node &node = nodes_[node_index];
        if (node.handler_count == 0) {
            return false;
        }
        const std::uint32_t begin = node.handler_begin;
        const std::uint32_t end = begin + node.handler_count;
        for (std::uint32_t i = begin; i < end; ++i) {
            if (context.matched(node.id, handlers_[i])) {
                return true;
            }
        }
        return false;
    }

    template<typename Context>
    [[nodiscard]] bool exec(std::string_view path, std::size_t idx, std::uint32_t node_index, Context &context) const {
        const std::size_t length = path.size();
        std::size_t begin = idx;
        std::size_t end = idx;
        std::uint32_t hash = 0;
        for (; end < length; ++end) {
            const unsigned char ch = static_cast<unsigned char>(path[end]);
            if (ch != '/') {
                hash = hash * 31u + ch;
            } else if (begin < end) {
                break;
            } else {
                ++begin;
            }
        }

        const Node &node = nodes_[node_index];
        const char *segment_data = path.data() + begin;
        const std::size_t segment_size = end - begin;
        const bool ends = end >= length;

        const std::uint32_t static_child = find_static_child(node, segment_data, segment_size, hash);
        if (static_child != kInvalidIndex) {
            if (ends) {
                if (match_node(static_child, context)) {
                    return true;
                }
                if (end > begin && exec(path, end, static_child, context)) {
                    return true;
                }
            } else if (exec(path, end, static_child, context)) {
                return true;
            }
        }

        if ((end > begin || begin > idx) && node.placeholder_count != 0) {
            const std::string_view value(segment_data, segment_size);
            const std::uint32_t placeholder_end = node.placeholder_begin + node.placeholder_count;
            for (std::uint32_t i = node.placeholder_begin; i < placeholder_end; ++i) {
                const std::uint32_t child_index = placeholder_children_[i];
                const Node &child = nodes_[child_index];
                const bool has_param = child.name_size != 0;
                if (has_param) {
                    context.add_path_var(node_name_view(child), value);
                }
                if (ends) {
                    if (match_node(child_index, context)) {
                        return true;
                    }
                    if (end > begin && exec(path, end, child_index, context)) {
                        return true;
                    }
                } else if (exec(path, end, child_index, context)) {
                    return true;
                }
                if (has_param) {
                    context.pop_path_var();
                }
            }
        }

        if (node.wildcard_count != 0) {
            const std::string_view value(path.data() + begin, length - begin);
            const std::uint32_t wildcard_end = node.wildcard_begin + node.wildcard_count;
            for (std::uint32_t i = node.wildcard_begin; i < wildcard_end; ++i) {
                const std::uint32_t child_index = wildcard_children_[i];
                const Node &child = nodes_[child_index];
                const bool has_param = child.name_size != 0;
                if (has_param) {
                    context.add_path_var(node_name_view(child), value);
                }
                if (match_node(child_index, context)) {
                    return true;
                }
                if (has_param) {
                    context.pop_path_var();
                }
            }
        }

        return end <= begin && begin > idx && match_node(node_index, context);
    }

    std::vector<Node> nodes_{};
    std::vector<std::uint32_t> static_slots_{};
    std::vector<std::uint32_t> placeholder_children_{};
    std::vector<std::uint32_t> wildcard_children_{};
    std::vector<Handler> handlers_{};
    std::vector<char> text_{};
    std::uint32_t max_path_var_count_{0};
};

template<typename Handler>
template<typename BuilderPayload, typename RouteVarDefiner>
class RoutePathMatcher<Handler>::Builder {
public:
    // RouteVarDefiner::add_path_var_definer receives a transient var_name view.
    // The view is only valid for the duration of the callback and must be copied
    // by the definer if it needs to retain the name after the call returns.
    explicit Builder(RouteVarDefiner &route_definer) : route_definer_(route_definer) {
        nodes_.reserve(16);
        nodes_.push_back(BuildNode{});
    }

    void add_route(std::string_view pattern, BuilderPayload payload) {
        ensure_mutable();
        validate_ascii_pattern(pattern);

        payloads_.push_back(std::move(payload));
        const std::uint32_t payload_index = static_cast<std::uint32_t>(payloads_.size() - 1);
        const std::uint32_t node_index = add_path(pattern, payloads_[payload_index]);
        BuildNode &node = nodes_[node_index];
        if (node.id == kInvalidIndex) {
            node.id = next_node_id_++;
        }
        node.mounted_routes.push_back({
                .payload_index = payload_index,
                .full_path_offset = append_text(pattern.data(), pattern.size()),
                .full_path_size = static_cast<std::uint32_t>(pattern.size()),
        });
    }

    [[nodiscard]] RoutePathMatcher build() {
        ensure_mutable();
        complete_ = true;

        RoutePathMatcher matcher;
        matcher.nodes_.resize(nodes_.size());
        matcher.text_ = std::move(text_);
        matcher.handlers_.reserve(payloads_.size());
        matcher.max_path_var_count_ = max_path_var_count_;

        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            const BuildNode &src = nodes_[i];
            Node &dst = matcher.nodes_[i];
            dst.name_offset = src.name_offset;
            dst.name_size = src.name_size;
            dst.hash = src.hash;
            dst.id = src.id;

            if (!src.static_slots.empty()) {
                dst.static_slot_begin = static_cast<std::uint32_t>(matcher.static_slots_.size());
                dst.static_slot_count = static_cast<std::uint32_t>(src.static_slots.size());
                matcher.static_slots_.insert(matcher.static_slots_.end(), src.static_slots.begin(),
                                             src.static_slots.end());
            }

            if (!src.placeholder_children.empty()) {
                dst.placeholder_begin = static_cast<std::uint32_t>(matcher.placeholder_children_.size());
                dst.placeholder_count = static_cast<std::uint32_t>(src.placeholder_children.size());
                matcher.placeholder_children_.insert(matcher.placeholder_children_.end(),
                                                     src.placeholder_children.begin(), src.placeholder_children.end());
            }

            if (!src.wildcard_children.empty()) {
                dst.wildcard_begin = static_cast<std::uint32_t>(matcher.wildcard_children_.size());
                dst.wildcard_count = static_cast<std::uint32_t>(src.wildcard_children.size());
                matcher.wildcard_children_.insert(matcher.wildcard_children_.end(), src.wildcard_children.begin(),
                                                  src.wildcard_children.end());
            }

            if (!src.mounted_routes.empty()) {
                dst.handler_begin = static_cast<std::uint32_t>(matcher.handlers_.size());
                dst.handler_count = static_cast<std::uint32_t>(src.mounted_routes.size());
                for (const MountedRoute &route: src.mounted_routes) {
                    const std::string_view full_path(matcher.text_.data() + route.full_path_offset,
                                                     route.full_path_size);
                    matcher.handlers_.push_back(
                            route_definer_.on_route_mount(dst.id, full_path, payloads_[route.payload_index]));
                }
            }
        }

        return matcher;
    }

private:
    struct MountedRoute {
        std::uint32_t payload_index = 0;
        std::uint32_t full_path_offset = 0;
        std::uint32_t full_path_size = 0;
    };

    struct BuildNode {
        std::uint32_t name_offset = 0;
        std::uint32_t name_size = 0;
        std::uint32_t hash = 0;
        std::uint32_t id = kInvalidIndex;
        std::vector<std::uint32_t> static_slots{};
        std::uint32_t static_child_count = 0;
        std::vector<std::uint32_t> placeholder_children{};
        std::vector<std::uint32_t> wildcard_children{};
        std::vector<MountedRoute> mounted_routes{};
    };

    void ensure_mutable() const {
        if (complete_) {
            throw std::logic_error("route matcher builder already completed");
        }
    }

    static void validate_ascii_pattern(std::string_view pattern) {
        if (pattern.empty()) {
            throw RoutePatternError("empty path pattern is not allowed");
        }
        for (const unsigned char ch: pattern) {
            if ((ch & 0x80u) != 0) {
                throw RoutePatternError("path pattern must use ASCII bytes only");
            }
        }
    }

    [[nodiscard]] std::uint32_t append_text(const char *data, std::size_t size) {
        const std::uint32_t offset = static_cast<std::uint32_t>(text_.size());
        text_.insert(text_.end(), data, data + size);
        return offset;
    }

    [[nodiscard]] std::string_view node_name_view(std::uint32_t node_index) const noexcept {
        const BuildNode &node = nodes_[node_index];
        return std::string_view(text_.data() + node.name_offset, node.name_size);
    }

    [[nodiscard]] bool node_name_equals(const BuildNode &node, const char *segment_data, std::size_t segment_size,
                                        std::uint32_t hash) const noexcept {
        if (node.hash != hash || node.name_size != segment_size) {
            return false;
        }
        const char *name = text_.data() + node.name_offset;
        for (std::size_t i = 0; i < segment_size; ++i) {
            if (name[i] != segment_data[i]) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::uint32_t create_node(const char *segment_data, std::size_t segment_size, std::uint32_t hash) {
        BuildNode node;
        node.name_offset = append_text(segment_data, segment_size);
        node.name_size = static_cast<std::uint32_t>(segment_size);
        node.hash = hash;
        nodes_.push_back(std::move(node));
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }

    static void fill_invalid(std::vector<std::uint32_t> &slots, std::size_t count) {
        slots.assign(count, kInvalidIndex);
    }

    void rehash_static_slots(BuildNode &node, std::size_t new_size) {
        std::vector<std::uint32_t> rehashed;
        fill_invalid(rehashed, new_size);
        for (std::uint32_t child_index: node.static_slots) {
            if (child_index == kInvalidIndex) {
                continue;
            }
            const BuildNode &child = nodes_[child_index];
            std::uint32_t slot = child.hash & (static_cast<std::uint32_t>(new_size) - 1);
            while (rehashed[slot] != kInvalidIndex) {
                slot = (slot + 1) & (static_cast<std::uint32_t>(new_size) - 1);
            }
            rehashed[slot] = child_index;
        }
        node.static_slots = std::move(rehashed);
    }

    [[nodiscard]] std::uint32_t add_or_get_static_child(std::uint32_t node_index, const char *segment_data,
                                                        std::size_t segment_size, std::uint32_t hash) {
        if (nodes_[node_index].static_slots.empty()) {
            fill_invalid(nodes_[node_index].static_slots, 8);
        } else if ((nodes_[node_index].static_child_count << 1u) > nodes_[node_index].static_slots.size()) {
            rehash_static_slots(nodes_[node_index], nodes_[node_index].static_slots.size() << 1u);
        }

        const std::uint32_t slot_mask = static_cast<std::uint32_t>(nodes_[node_index].static_slots.size() - 1);
        std::uint32_t slot = hash & slot_mask;
        while (true) {
            const std::uint32_t child_index = nodes_[node_index].static_slots[slot];
            if (child_index == kInvalidIndex) {
                const std::uint32_t created = create_node(segment_data, segment_size, hash);
                nodes_[node_index].static_slots[slot] = created;
                ++nodes_[node_index].static_child_count;
                return created;
            }
            if (node_name_equals(nodes_[child_index], segment_data, segment_size, hash)) {
                return child_index;
            }
            slot = (slot + 1) & slot_mask;
        }
    }

    [[nodiscard]] std::uint32_t add_or_get_placeholder(std::uint32_t node_index, const char *segment_data,
                                                       std::size_t segment_size, std::uint32_t hash) {
        for (std::uint32_t child_index: nodes_[node_index].placeholder_children) {
            if (node_name_equals(nodes_[child_index], segment_data, segment_size, hash)) {
                return child_index;
            }
        }
        const std::uint32_t created = create_node(segment_data, segment_size, hash);
        nodes_[node_index].placeholder_children.push_back(created);
        return created;
    }

    [[nodiscard]] std::uint32_t add_or_get_wildcard(std::uint32_t node_index, const char *segment_data,
                                                    std::size_t segment_size, std::uint32_t hash) {
        for (std::uint32_t child_index: nodes_[node_index].wildcard_children) {
            if (node_name_equals(nodes_[child_index], segment_data, segment_size, hash)) {
                return child_index;
            }
        }
        const std::uint32_t created = create_node(segment_data, segment_size, hash);
        nodes_[node_index].wildcard_children.push_back(created);
        return created;
    }

    [[nodiscard]] std::uint32_t add_path(std::string_view pattern, BuilderPayload &payload) {
        const char *chars = pattern.data();
        const std::size_t length = pattern.size();

        std::uint32_t node_index = 0;
        std::uint32_t path_var_index = 0;
        std::uint32_t hash = 0;
        std::size_t segment_start = 0;
        unsigned wild = 0;

        for (std::size_t i = 0; i <= length; ++i) {
            const unsigned char ch = i < length ? static_cast<unsigned char>(chars[i]) : 0;
            if (ch == 0 || ch == '/') {
                std::uint32_t child_index = node_index;
                if (wild == 2) {
                    if (ch != 0) {
                        throw RoutePatternError("wildcard segment must be the last path segment");
                    }
                    child_index =
                            add_or_get_wildcard(node_index, chars + segment_start + 1, i - segment_start - 1, hash);
                    if (segment_start + 1 < i) {
                        route_definer_.add_path_var_definer(payload, node_name_view(child_index), path_var_index++);
                    }
                } else if (wild == 1) {
                    child_index =
                            add_or_get_placeholder(node_index, chars + segment_start + 1, i - segment_start - 1, hash);
                    if (segment_start + 1 < i) {
                        route_definer_.add_path_var_definer(payload, node_name_view(child_index), path_var_index++);
                    }
                } else if (i > 0) {
                    if (segment_start < i || segment_start == length) {
                        child_index =
                                add_or_get_static_child(node_index, chars + segment_start, i - segment_start, hash);
                    }
                } else {
                    child_index = 0;
                }

                node_index = child_index;
                hash = 0;
                segment_start = i + 1;
                wild = 0;
                continue;
            }

            if (segment_start != i || (ch != ':' && ch != '*')) {
                hash = hash * 31u + ch;
            } else {
                wild = ch == ':' ? 1u : 2u;
            }
        }

        max_path_var_count_ = std::max(max_path_var_count_, path_var_index);
        return node_index;
    }

    RouteVarDefiner &route_definer_;
    std::vector<BuildNode> nodes_{};
    std::vector<BuilderPayload> payloads_{};
    std::vector<char> text_{};
    std::uint32_t max_path_var_count_{0};
    std::uint32_t next_node_id_{0};
    bool complete_{false};
};

} // namespace fiber::util

#endif // FIBER_UTIL_ROUTE_PATH_MATCHER_H
