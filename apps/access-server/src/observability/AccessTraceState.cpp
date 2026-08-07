#include "AccessTraceState.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include <fiber/common/mem/BufPool.h>

namespace fiber::access_server {
namespace {

constexpr std::string_view kBnrc = "bnrc";
constexpr std::string_view kBase62Alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

bool checked_add(std::size_t left, std::size_t right, std::size_t &result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

bool checked_twice(std::size_t value, std::size_t &result) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() / 2) {
        return false;
    }
    result = value * 2;
    return true;
}

bool is_ows(char value) noexcept { return value == ' ' || value == '\t'; }

std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() && is_ows(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ows(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

int base62_digit(unsigned char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'Z') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 36;
    }
    return -1;
}

common::IoResult<std::string_view> decode_base62(mem::BufPool &pool, std::string_view encoded) noexcept {
    if (encoded.empty()) {
        return std::string_view{};
    }
    std::size_t capacity = 0;
    if (!checked_twice(encoded.size(), capacity)) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    auto *data = pool.alloc<std::uint8_t>(capacity);
    if (!data) {
        return std::unexpected(common::IoErr::NoMem);
    }
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const int digit = base62_digit(static_cast<unsigned char>(encoded[i]));
        if (digit < 0) {
            return std::unexpected(common::IoErr::Invalid);
        }
        data[i] = static_cast<std::uint8_t>(digit);
    }

    const std::size_t output_begin = encoded.size();
    std::size_t output_end = output_begin;
    std::size_t source_end = encoded.size();
    while (source_end != 0) {
        std::uint32_t remainder = 0;
        std::size_t target = 0;
        bool has_value = false;
        for (std::size_t i = 0; i < source_end; ++i) {
            const std::uint32_t accumulator = remainder * 62U + data[i];
            const std::uint32_t digit = accumulator / 256U;
            remainder = accumulator % 256U;
            if (digit != 0 || has_value) {
                data[target++] = static_cast<std::uint8_t>(digit);
                has_value = true;
            }
        }
        data[output_end++] = static_cast<std::uint8_t>(remainder);
        source_end = target;
    }
    std::reverse(data + output_begin, data + output_end);
    return std::string_view(reinterpret_cast<const char *>(data + output_begin), output_end - output_begin);
}

std::size_t encode_base62(std::string_view value, std::uint8_t *scratch, char *output) noexcept {
    if (value.empty()) {
        return 0;
    }
    std::memcpy(scratch, value.data(), value.size());
    std::size_t source_end = value.size();
    std::size_t output_size = 0;
    while (source_end != 0) {
        std::uint32_t remainder = 0;
        std::size_t target = 0;
        bool has_value = false;
        for (std::size_t i = 0; i < source_end; ++i) {
            const std::uint32_t accumulator = remainder * 256U + scratch[i];
            const std::uint32_t digit = accumulator / 62U;
            remainder = accumulator % 62U;
            if (digit != 0 || has_value) {
                scratch[target++] = static_cast<std::uint8_t>(digit);
                has_value = true;
            }
        }
        output[output_size++] = kBase62Alphabet[remainder];
        source_end = target;
    }
    std::reverse(output, output + output_size);
    return output_size;
}

} // namespace

struct AccessTraceState::StateMember {
    std::string_view key;
    std::string_view value;
    StateMember *next = nullptr;
};

struct AccessTraceState::ContextEntry {
    std::string_view key;
    std::string_view value;
    ContextEntry *next = nullptr;
};

AccessTraceState::AccessTraceState(mem::BufPool &pool) noexcept : pool_(pool) {}

void AccessTraceState::parse(std::string_view trace_state) noexcept {
    inbound_ = trace_state;
    if (trace_state.empty()) {
        return;
    }

    std::string_view bnrc_value;
    bool has_bnrc = false;
    std::size_t offset = 0;
    while (offset <= trace_state.size()) {
        const std::size_t comma = trace_state.find(',', offset);
        const std::size_t end = comma == std::string_view::npos ? trace_state.size() : comma;
        const std::string_view member = trim_ows(trace_state.substr(offset, end - offset));
        const std::size_t equals = member.find('=');
        if (member.empty() || equals == std::string_view::npos) {
            invalidate_parse();
            return;
        }
        parsed_member_ = true;
        const std::string_view key = member.substr(0, equals);
        const std::string_view value = member.substr(equals + 1);
        if (key == kBnrc) {
            has_bnrc = true;
            bnrc_value = value;
        } else if (!append_state_member(key, value)) {
            invalidate_parse();
            return;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        offset = comma + 1;
    }

    if (!has_bnrc || bnrc_value.empty()) {
        return;
    }
    std::size_t context_offset = 0;
    while (context_offset < bnrc_value.size()) {
        const std::size_t key_end = bnrc_value.find('-', context_offset);
        if (key_end == std::string_view::npos) {
            break;
        }
        const std::size_t value_end = bnrc_value.find('-', key_end + 1);
        const std::size_t pair_end = value_end == std::string_view::npos ? bnrc_value.size() : value_end;
        auto key = decode_base62(pool_, bnrc_value.substr(context_offset, key_end - context_offset));
        auto value = decode_base62(pool_, bnrc_value.substr(key_end + 1, pair_end - key_end - 1));
        if (!key || !value || !put_context_view(*key, *value)) {
            invalidate_parse();
            return;
        }
        if (value_end == std::string_view::npos) {
            break;
        }
        context_offset = value_end + 1;
    }
}

bool AccessTraceState::should_override_upstream() const noexcept {
    return rebuildable_ && (parsed_member_ || context_count_ != 0);
}

std::optional<std::string_view> AccessTraceState::get_context(std::string_view key) const noexcept {
    for (const ContextEntry *entry = context_head_; entry; entry = entry->next) {
        if (entry->key == key) {
            return entry->value;
        }
    }
    return std::nullopt;
}

common::IoResult<void> AccessTraceState::put_context(std::string_view key, std::string_view value) noexcept {
    ContextEntry *existing = nullptr;
    for (ContextEntry *entry = context_head_; entry; entry = entry->next) {
        if (entry->key == key) {
            existing = entry;
            break;
        }
    }
    if (existing && existing->value == value) {
        return {};
    }

    std::size_t bytes = value.size();
    if (!existing && !checked_add(bytes, key.size(), bytes)) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    char *copy = bytes == 0 ? nullptr : pool_.alloc<char>(bytes);
    if (bytes != 0 && !copy) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (existing) {
        if (!value.empty()) {
            std::memcpy(copy, value.data(), value.size());
        }
        existing->value = value.empty() ? std::string_view{} : std::string_view(copy, value.size());
        dirty_ = true;
        return {};
    }

    if (!key.empty()) {
        std::memcpy(copy, key.data(), key.size());
    }
    if (!value.empty()) {
        std::memcpy(copy + key.size(), value.data(), value.size());
    }
    const std::string_view copied_key = key.empty() ? std::string_view{} : std::string_view(copy, key.size());
    const std::string_view copied_value =
            value.empty() ? std::string_view{} : std::string_view(copy + key.size(), value.size());
    if (!put_context_view(copied_key, copied_value)) {
        return std::unexpected(common::IoErr::NoMem);
    }
    return {};
}

bool AccessTraceState::remove_context(std::string_view key) noexcept {
    ContextEntry *previous = nullptr;
    for (ContextEntry *entry = context_head_; entry; entry = entry->next) {
        if (entry->key != key) {
            previous = entry;
            continue;
        }
        if (previous) {
            previous->next = entry->next;
        } else {
            context_head_ = entry->next;
        }
        if (context_tail_ == entry) {
            context_tail_ = previous;
        }
        --context_count_;
        dirty_ = true;
        return true;
    }
    return false;
}

common::IoResult<std::string_view> AccessTraceState::encode() noexcept {
    if (!should_override_upstream()) {
        return std::string_view{};
    }
    if (!dirty_) {
        return encoded_;
    }

    std::size_t capacity = 0;
    std::size_t members = 0;
    for (const StateMember *member = state_head_; member; member = member->next) {
        if (members != 0 && !checked_add(capacity, 1, capacity)) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        if (!checked_add(capacity, member->key.size(), capacity) || !checked_add(capacity, 1, capacity) ||
            !checked_add(capacity, member->value.size(), capacity)) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        ++members;
    }
    if (members != 0 && !checked_add(capacity, 1, capacity)) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }
    if (!checked_add(capacity, kBnrc.size() + 1, capacity)) {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    std::size_t max_context_bytes = 0;
    std::size_t context_index = 0;
    for (const ContextEntry *entry = context_head_; entry; entry = entry->next) {
        std::size_t key_capacity = 0;
        std::size_t value_capacity = 0;
        if (!checked_twice(entry->key.size(), key_capacity) || !checked_twice(entry->value.size(), value_capacity) ||
            !checked_add(capacity, key_capacity, capacity) || !checked_add(capacity, value_capacity, capacity)) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        if (context_index != 0 && !checked_add(capacity, 1, capacity)) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        if (!checked_add(capacity, 1, capacity)) {
            return std::unexpected(common::IoErr::MessageTooLarge);
        }
        max_context_bytes = std::max(max_context_bytes, std::max(entry->key.size(), entry->value.size()));
        ++context_index;
    }

    char *output = pool_.alloc<char>(capacity);
    if (!output) {
        return std::unexpected(common::IoErr::NoMem);
    }
    std::uint8_t *scratch = max_context_bytes == 0 ? nullptr : pool_.alloc<std::uint8_t>(max_context_bytes);
    if (max_context_bytes != 0 && !scratch) {
        return std::unexpected(common::IoErr::NoMem);
    }

    char *cursor = output;
    members = 0;
    for (const StateMember *member = state_head_; member; member = member->next) {
        if (members++ != 0) {
            *cursor++ = ',';
        }
        if (!member->key.empty()) {
            std::memcpy(cursor, member->key.data(), member->key.size());
            cursor += member->key.size();
        }
        *cursor++ = '=';
        if (!member->value.empty()) {
            std::memcpy(cursor, member->value.data(), member->value.size());
            cursor += member->value.size();
        }
    }
    if (members != 0) {
        *cursor++ = ',';
    }
    std::memcpy(cursor, kBnrc.data(), kBnrc.size());
    cursor += kBnrc.size();
    *cursor++ = '=';

    context_index = 0;
    for (const ContextEntry *entry = context_head_; entry; entry = entry->next) {
        if (context_index++ != 0) {
            *cursor++ = '-';
        }
        cursor += encode_base62(entry->key, scratch, cursor);
        *cursor++ = '-';
        cursor += encode_base62(entry->value, scratch, cursor);
    }
    encoded_ = std::string_view(output, static_cast<std::size_t>(cursor - output));
    dirty_ = false;
    return encoded_;
}

bool AccessTraceState::append_state_member(std::string_view key, std::string_view value) noexcept {
    for (StateMember *entry = state_head_; entry; entry = entry->next) {
        if (entry->key == key) {
            entry->value = value;
            return true;
        }
    }
    auto *storage = pool_.alloc<StateMember>();
    if (!storage) {
        return false;
    }
    auto *entry = new (storage) StateMember{.key = key, .value = value};
    if (state_tail_) {
        state_tail_->next = entry;
    } else {
        state_head_ = entry;
    }
    state_tail_ = entry;
    return true;
}

bool AccessTraceState::put_context_view(std::string_view key, std::string_view value) noexcept {
    for (ContextEntry *entry = context_head_; entry; entry = entry->next) {
        if (entry->key == key) {
            entry->value = value;
            dirty_ = true;
            return true;
        }
    }
    auto *storage = pool_.alloc<ContextEntry>();
    if (!storage) {
        return false;
    }
    auto *entry = new (storage) ContextEntry{.key = key, .value = value};
    if (context_tail_) {
        context_tail_->next = entry;
    } else {
        context_head_ = entry;
    }
    context_tail_ = entry;
    ++context_count_;
    dirty_ = true;
    return true;
}

void AccessTraceState::invalidate_parse() noexcept {
    state_head_ = nullptr;
    state_tail_ = nullptr;
    context_head_ = nullptr;
    context_tail_ = nullptr;
    context_count_ = 0;
    parsed_member_ = false;
    rebuildable_ = false;
    encoded_ = {};
    dirty_ = true;
}

void AccessTraceState::for_each_context_impl(void *opaque, ContextVisitorFn visitor) const noexcept {
    if (!visitor) {
        return;
    }
    for (const ContextEntry *entry = context_head_; entry; entry = entry->next) {
        if (!visitor(opaque, entry->key, entry->value)) {
            return;
        }
    }
}

} // namespace fiber::access_server
