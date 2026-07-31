#include "HostMatcher.h"

#include <algorithm>
#include <limits>

namespace fiber::access_server {
namespace {

constexpr std::uint32_t kNoNode = std::numeric_limits<std::uint32_t>::max();

char java_host_fold(char value) noexcept { return static_cast<char>(static_cast<unsigned char>(value) | 0x20U); }

bool label_equals(std::string_view left_lower, std::string_view right) noexcept {
    if (left_lower.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left_lower.size(); ++i) {
        if (left_lower[i] != java_host_fold(right[i])) {
            return false;
        }
    }
    return true;
}

std::string lower_copy(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (char ch: value) {
        result.push_back(java_host_fold(ch));
    }
    return result;
}

AccessConfigError host_error(std::string_view message) {
    return AccessConfigError{
            .code = AccessConfigErrorCode::InvalidField,
            .field = "host",
            .message = std::string(message),
    };
}

std::optional<std::size_t> validated_host_end(std::string_view host) noexcept {
    if (host.empty()) {
        return std::nullopt;
    }

    enum class State : std::uint8_t {
        Usual,
        Literal,
        Rest,
    };

    std::size_t host_end = host.size();
    std::size_t dot_position = host.size();
    State state = State::Usual;
    for (std::size_t i = 0; i < host.size(); ++i) {
        const auto ch = static_cast<unsigned char>(host[i]);
        switch (ch) {
            case '.':
                if (i != 0 && dot_position == i - 1) {
                    return std::nullopt;
                }
                if (i == 0 && dot_position == std::numeric_limits<std::size_t>::max()) {
                    return std::nullopt;
                }
                dot_position = i;
                break;
            case ':':
                if (state == State::Usual) {
                    host_end = i;
                    state = State::Rest;
                }
                break;
            case '[':
                if (i == 0) {
                    state = State::Literal;
                }
                break;
            case ']':
                if (state == State::Literal) {
                    host_end = i + 1;
                    state = State::Rest;
                }
                break;
            default:
                if (ch == '/' || ch <= 0x20U || ch == 0x7FU) {
                    return std::nullopt;
                }
                break;
        }
    }

    if (dot_position == host_end - 1) {
        --host_end;
    }
    if (host_end == 0) {
        return std::nullopt;
    }
    return host_end;
}

} // namespace

HostMatcher::HostMatcher() { nodes_.emplace_back(); }

std::expected<HostMatcher, AccessConfigError> HostMatcher::build(std::span<const HostPattern> patterns) {
    HostMatcher matcher;
    for (const HostPattern &item: patterns) {
        const std::string_view pattern = item.pattern;
        if (pattern.empty()) {
            // AccessRouteConfigWatcher skips empty map keys.
            continue;
        }

        std::uint32_t node = 0;
        std::size_t label_end = pattern.size();
        bool wildcard = false;
        for (std::size_t offset = pattern.size(); offset > 0; --offset) {
            const std::size_t i = offset - 1;
            if (pattern[i] == '.') {
                node = matcher.add_or_find_child(node, pattern.substr(i + 1, label_end - i - 1));
                label_end = i;
            } else if (pattern[i] == '*') {
                if (matcher.nodes_[node].wildcard_handler != kNoHandler) {
                    return std::unexpected(host_error("wildcard is duplicate"));
                }
                if (i != 0 || label_end != 1) {
                    return std::unexpected(host_error("* must be the complete first host label"));
                }
                matcher.nodes_[node].wildcard_handler = item.handler;
                wildcard = true;
                break;
            }
        }

        if (!wildcard) {
            node = matcher.add_or_find_child(node, pattern.substr(0, label_end));
            if (matcher.nodes_[node].exact_handler != kNoHandler) {
                return std::unexpected(host_error("host is duplicate"));
            }
            matcher.nodes_[node].exact_handler = item.handler;
        }
    }

    return matcher;
}

std::optional<std::uint32_t> HostMatcher::match(std::string_view host) const noexcept {
    const std::optional<std::size_t> end = validated_host_end(host);
    if (!end) {
        return std::nullopt;
    }
    const std::uint32_t handler = match_node(0, host, *end);
    if (handler == kNoHandler) {
        return std::nullopt;
    }
    return handler;
}

bool HostMatcher::empty() const noexcept {
    return nodes_.size() == 1 && nodes_[0].children.empty() && nodes_[0].exact_handler == kNoHandler &&
           nodes_[0].wildcard_handler == kNoHandler;
}

std::uint32_t HostMatcher::find_child(std::uint32_t node, std::string_view label) const noexcept {
    for (const Child &child: nodes_[node].children) {
        if (label_equals(child.label, label)) {
            return child.node;
        }
    }
    return kNoNode;
}

std::uint32_t HostMatcher::add_or_find_child(std::uint32_t node, std::string_view label) {
    for (const Child &child: nodes_[node].children) {
        if (label_equals(child.label, label)) {
            return child.node;
        }
    }

    const std::uint32_t child_index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.emplace_back();
    nodes_[node].children.push_back(Child{
            .label = lower_copy(label),
            .node = child_index,
    });
    return child_index;
}

std::uint32_t HostMatcher::match_node(std::uint32_t node, std::string_view host, std::size_t end) const noexcept {
    std::size_t begin = end;
    while (begin > 0 && host[begin - 1] != '.') {
        --begin;
    }

    const std::uint32_t child = find_child(node, host.substr(begin, end - begin));
    if (child == kNoNode) {
        return nodes_[node].wildcard_handler;
    }
    if (begin <= 1) {
        return nodes_[child].exact_handler;
    }

    // Java WildHostNode deliberately does not fall back to this node's
    // wildcard when an exact child exists but its deeper match fails.
    return match_node(child, host, begin - 1);
}

} // namespace fiber::access_server
