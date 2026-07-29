#include "CatEncoder.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include "CatInternal.h"

namespace fiber::cat::detail {

namespace {

inline constexpr std::size_t kFramePrefixSize = 4;
inline constexpr std::string_view kNt1Version = "NT1";

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &result) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

class CountingWriter {
public:
    bool write_byte(std::uint8_t) noexcept { return add(1); }

    bool write_bytes(const char *data, std::size_t size) noexcept {
        if (size != 0 && data == nullptr) {
            return false;
        }
        return add(size);
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

private:
    bool add(std::size_t size) noexcept {
        std::size_t result = 0;
        if (!checked_add(size_, size, result)) {
            overflowed_ = true;
            return false;
        }
        size_ = result;
        return true;
    }

    std::size_t size_ = 0;
    bool overflowed_ = false;
};

class FixedWriter {
public:
    FixedWriter(std::uint8_t *begin, std::size_t size) noexcept : current_(begin), end_(begin + size) {}

    bool write_byte(std::uint8_t value) noexcept {
        if (current_ == end_) {
            return false;
        }
        *current_++ = value;
        return true;
    }

    bool write_bytes(const char *data, std::size_t size) noexcept {
        if ((size != 0 && data == nullptr) || size > remaining()) {
            return false;
        }
        if (size != 0) {
            std::memcpy(current_, data, size);
            current_ += size;
        }
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return static_cast<std::size_t>(end_ - current_); }

private:
    std::uint8_t *current_ = nullptr;
    std::uint8_t *end_ = nullptr;
};

template<typename Writer>
bool write_unsigned(Writer &writer, std::uint64_t value) noexcept {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        if (!writer.write_byte(byte)) {
            return false;
        }
    } while (value != 0);
    return true;
}

template<typename Writer>
bool write_string(Writer &writer, const char *data, std::size_t size) noexcept {
    return write_unsigned(writer, size) && writer.write_bytes(data, size);
}

template<typename Writer>
bool write_string(Writer &writer, std::string_view value) noexcept {
    return write_string(writer, value.data(), value.size());
}

template<typename Writer>
bool write_string(Writer &writer, StringRef value) noexcept {
    return write_string(writer, value.data, value.size);
}

bool message_timestamp(const MessageTraceData &trace, const MessageData &message, std::uint64_t &result) noexcept {
    using Milliseconds = std::chrono::milliseconds;

    const auto base = std::chrono::duration_cast<Milliseconds>(trace.steady_base.time_since_epoch()).count();
    const auto timestamp = std::chrono::duration_cast<Milliseconds>(message.time.time_since_epoch()).count();
    if (timestamp >= base) {
        const std::uint64_t delta = static_cast<std::uint64_t>(timestamp) - static_cast<std::uint64_t>(base);
        if (delta > std::numeric_limits<std::uint64_t>::max() - trace.wall_base_millis) {
            return false;
        }
        result = trace.wall_base_millis + delta;
        return true;
    }

    const std::uint64_t delta = static_cast<std::uint64_t>(base) - static_cast<std::uint64_t>(timestamp);
    if (delta > trace.wall_base_millis) {
        return false;
    }
    result = trace.wall_base_millis - delta;
    return true;
}

std::string_view truncation_reason(RecordError error) noexcept {
    switch (error) {
        case RecordError::LimitExceeded:
            return "limit";
        case RecordError::NoMemory:
            return "memory";
        default:
            return "record";
    }
}

std::string_view build_truncation_marker(const MessageTraceData &trace, std::array<char, 192> &storage) noexcept {
    std::size_t size = 0;
    const auto append = [&](std::string_view value) noexcept {
        if (value.size() > storage.size() - size) {
            return false;
        }
        std::copy(value.begin(), value.end(), storage.data() + size);
        size += value.size();
        return true;
    };
    const auto number = [&](std::uint64_t value) noexcept {
        auto result = std::to_chars(storage.data() + size, storage.data() + storage.size(), value);
        if (result.ec != std::errc{}) {
            return false;
        }
        size = static_cast<std::size_t>(result.ptr - storage.data());
        return true;
    };
    if (!append("CatClient.Truncated=count:") || !number(trace.dropped_message_count) || !append(",bytes:") ||
        !number(trace.dropped_data_bytes) || !append(",reason:") ||
        !append(truncation_reason(trace.first_truncation_reason))) {
        return {};
    }
    return {storage.data(), size};
}

template<typename Writer>
bool write_data_contents(Writer &writer, const MessageData &message, const MessageTraceData &trace) noexcept {
    std::array<char, 192> marker_storage{};
    const bool append_marker = trace.truncated && trace.root == &message;
    const std::string_view marker = append_marker ? build_truncation_marker(trace, marker_storage) : std::string_view{};
    if (append_marker && marker.empty()) {
        return false;
    }

    std::size_t observed = 0;
    for (const DataChunk *chunk = message.data_head; chunk; chunk = chunk->next) {
        if (observed > message.data_size || chunk->used > chunk->capacity ||
            chunk->used > message.data_size - observed || !writer.write_bytes(chunk->data(), chunk->used)) {
            return false;
        }
        observed += chunk->used;
    }
    if (observed != message.data_size) {
        return false;
    }
    return !append_marker ||
           ((!message.has_data || writer.write_byte(static_cast<std::uint8_t>(message.data_separator))) &&
            writer.write_bytes(marker.data(), marker.size()));
}

template<typename Writer>
bool write_data(Writer &writer, const MessageData &message, const MessageTraceData &trace) noexcept {
    std::array<char, 192> marker_storage{};
    const bool append_marker = trace.truncated && trace.root == &message;
    const std::string_view marker = append_marker ? build_truncation_marker(trace, marker_storage) : std::string_view{};
    const std::size_t separator = append_marker && message.has_data ? 1 : 0;
    std::size_t encoded_size = 0;
    return (!append_marker || !marker.empty()) && checked_add(message.data_size, separator, encoded_size) &&
           checked_add(encoded_size, marker.size(), encoded_size) && write_unsigned(writer, encoded_size) &&
           write_data_contents(writer, message, trace);
}

struct EncodeState {
    const MessageTraceData &trace;
    std::size_t visited = 0;
};

template<typename Writer>
bool write_message(Writer &writer, const MessageData &message, EncodeState &state) noexcept {
    if (++state.visited > state.trace.message_count || !message.completed || !message.trace ||
        message.trace->data != &state.trace) {
        return false;
    }

    std::uint64_t timestamp = 0;
    if (!message_timestamp(state.trace, message, timestamp)) {
        return false;
    }

    if (message.kind == MessageKind::Event || message.kind == MessageKind::Metric ||
        message.kind == MessageKind::Heartbeat) {
        const std::uint8_t kind = message.kind == MessageKind::Event    ? 'E'
                                  : message.kind == MessageKind::Metric ? 'M'
                                                                        : 'H';
        return writer.write_byte(kind) && write_unsigned(writer, timestamp) && write_string(writer, message.type) &&
               write_string(writer, message.name) && write_string(writer, message.status) &&
               write_data(writer, message, state.trace);
    }
    if (message.kind != MessageKind::Transaction) {
        return false;
    }

    const auto &transaction = static_cast<const TransactionData &>(message);
    if (transaction.duration.count() < 0 || !writer.write_byte('t') || !write_unsigned(writer, timestamp) ||
        !write_string(writer, message.type) || !write_string(writer, message.name)) {
        return false;
    }

    std::size_t child_index = 0;
    const ChildrenChunk *chunk = transaction.children_head;
    while (child_index < transaction.child_count) {
        if (!chunk) {
            return false;
        }
        const std::size_t chunk_children = std::min(kChildrenPerChunk, transaction.child_count - child_index);
        for (std::size_t index = 0; index < chunk_children; ++index) {
            const MessageData *child = chunk->children[index];
            if (!child || !write_message(writer, *child, state)) {
                return false;
            }
        }
        child_index += chunk_children;
        chunk = chunk->next;
    }

    return writer.write_byte('T') && write_string(writer, message.status) && write_data(writer, message, state.trace) &&
           write_unsigned(writer, static_cast<std::uint64_t>(transaction.duration.count()));
}

template<typename Writer>
bool write_payload(Writer &writer, const MessageTraceData &trace, const ClientEncodeContext &client) noexcept {
    if (!trace.root || trace.open_message_count != 0 || trace.message_count == 0 ||
        !writer.write_bytes(kNt1Version.data(), kNt1Version.size()) || !write_string(writer, client.app_key) ||
        !write_string(writer, client.hostname) || !write_string(writer, client.ip) ||
        !write_string(writer, client.thread_group_name) || !write_string(writer, client.thread_id) ||
        !write_string(writer, client.thread_name) || !write_string(writer, trace.message_id) ||
        !write_string(writer, trace.parent_message_id) || !write_string(writer, trace.root_message_id) ||
        !write_string(writer, trace.session_token)) {
        return false;
    }

    EncodeState state{trace};
    return write_message(writer, *trace.root, state) && state.visited == trace.message_count;
}

template<typename Writer>
bool write_pt1_field(Writer &writer, std::string_view value) noexcept {
    return writer.write_bytes(value.data(), value.size());
}

template<typename Writer>
bool write_decimal(Writer &writer, std::uint64_t value) noexcept {
    std::array<char, 32> storage{};
    auto result = std::to_chars(storage.data(), storage.data() + storage.size(), value);
    return result.ec == std::errc{} &&
           writer.write_bytes(storage.data(), static_cast<std::size_t>(result.ptr - storage.data()));
}

template<typename Writer>
bool write_pt1_time(Writer &writer, std::uint64_t timestamp_millis) noexcept {
    const std::uint64_t seconds = timestamp_millis / 1000;
    if (seconds > static_cast<std::uint64_t>(std::numeric_limits<std::time_t>::max())) {
        return false;
    }
    const std::time_t value = static_cast<std::time_t>(seconds);
    std::tm local{};
    if (!::localtime_r(&value, &local)) {
        return false;
    }
    std::array<char, 32> storage{};
    const std::size_t prefix = std::strftime(storage.data(), storage.size(), "%Y-%m-%d %H:%M:%S", &local);
    if (prefix == 0 || prefix + 4 > storage.size()) {
        return false;
    }
    const int written = std::snprintf(storage.data() + prefix, storage.size() - prefix, ".%03u",
                                      static_cast<unsigned>(timestamp_millis % 1000));
    return written == 4 && writer.write_bytes(storage.data(), prefix + 4);
}

enum class Pt1Policy : std::uint8_t {
    Default,
    WithoutStatus,
    WithDuration,
};

template<typename Writer>
bool write_pt1_line(Writer &writer, const MessageTraceData &trace, const MessageData &message, std::uint8_t code,
                    Pt1Policy policy) noexcept {
    std::uint64_t timestamp = 0;
    if (!message_timestamp(trace, message, timestamp)) {
        return false;
    }
    const TransactionData *transaction = nullptr;
    if (message.kind == MessageKind::Transaction) {
        transaction = &static_cast<const TransactionData &>(message);
        if (transaction->duration.count() < 0) {
            return false;
        }
        if (code == 'T') {
            const std::uint64_t duration_millis = static_cast<std::uint64_t>(transaction->duration.count()) / 1000;
            if (duration_millis > std::numeric_limits<std::uint64_t>::max() - timestamp) {
                return false;
            }
            timestamp += duration_millis;
        }
    }
    if (!writer.write_byte(code) || !write_pt1_time(writer, timestamp) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, message.type.view()) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, message.name.view()) || !writer.write_byte('\t')) {
        return false;
    }
    if (policy == Pt1Policy::WithoutStatus) {
        return writer.write_byte('\n');
    }
    if (!write_pt1_field(writer, message.status.view()) || !writer.write_byte('\t')) {
        return false;
    }
    if (policy == Pt1Policy::WithDuration &&
        (!transaction || !write_decimal(writer, static_cast<std::uint64_t>(transaction->duration.count())) ||
         !writer.write_bytes("us\t", 3))) {
        return false;
    }
    return write_data_contents(writer, message, trace) && writer.write_byte('\t') && writer.write_byte('\n');
}

template<typename Writer>
bool write_pt1_message(Writer &writer, const MessageData &message, EncodeState &state) noexcept {
    if (++state.visited > state.trace.message_count || !message.completed || !message.trace ||
        message.trace->data != &state.trace) {
        return false;
    }
    if (message.kind != MessageKind::Transaction) {
        const std::uint8_t code = message.kind == MessageKind::Event       ? 'E'
                                  : message.kind == MessageKind::Metric    ? 'M'
                                  : message.kind == MessageKind::Heartbeat ? 'H'
                                                                           : 0;
        return code != 0 && write_pt1_line(writer, state.trace, message, code, Pt1Policy::Default);
    }

    const auto &transaction = static_cast<const TransactionData &>(message);
    if (transaction.child_count == 0) {
        return write_pt1_line(writer, state.trace, message, 'A', Pt1Policy::WithDuration);
    }
    if (!write_pt1_line(writer, state.trace, message, 't', Pt1Policy::WithoutStatus)) {
        return false;
    }
    std::size_t visited = 0;
    for (const ChildrenChunk *chunk = transaction.children_head; chunk; chunk = chunk->next) {
        const std::size_t count = std::min(kChildrenPerChunk, transaction.child_count - visited);
        for (std::size_t index = 0; index < count; ++index) {
            if (!chunk->children[index] || !write_pt1_message(writer, *chunk->children[index], state)) {
                return false;
            }
        }
        visited += count;
    }
    return visited == transaction.child_count &&
           write_pt1_line(writer, state.trace, message, 'T', Pt1Policy::WithDuration);
}

template<typename Writer>
bool write_pt1_payload(Writer &writer, const MessageTraceData &trace, const ClientEncodeContext &client) noexcept {
    if (!trace.root || trace.open_message_count != 0 || trace.message_count == 0 || !writer.write_bytes("PT1\t", 4) ||
        !write_pt1_field(writer, client.app_key) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, client.hostname) || !writer.write_byte('\t') || !write_pt1_field(writer, client.ip) ||
        !writer.write_byte('\t') || !write_pt1_field(writer, client.thread_group_name) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, client.thread_id) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, client.thread_name) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, trace.message_id.view()) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, trace.parent_message_id.view()) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, trace.root_message_id.view()) || !writer.write_byte('\t') ||
        !write_pt1_field(writer, trace.session_token.view()) || !writer.write_byte('\n')) {
        return false;
    }
    EncodeState state{trace};
    return write_pt1_message(writer, *trace.root, state) && state.visited == trace.message_count;
}

} // namespace

std::expected<mem::IoBuf, EncodeError> encode_nt1(const MessageTraceData &trace,
                                                  const ClientEncodeContext &client) noexcept {
    CountingWriter counter;
    if (!write_payload(counter, trace, client)) {
        return std::unexpected(counter.overflowed() ? EncodeError::SizeOverflow : EncodeError::InvalidTrace);
    }
    if (counter.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(EncodeError::SizeOverflow);
    }

    std::size_t frame_size = 0;
    if (!checked_add(kFramePrefixSize, counter.size(), frame_size)) {
        return std::unexpected(EncodeError::SizeOverflow);
    }
    mem::IoBuf result = mem::IoBuf::allocate(frame_size);
    if (!result) {
        return std::unexpected(EncodeError::NoMemory);
    }

    FixedWriter writer(result.writable_data(), frame_size);
    const auto payload_size = static_cast<std::uint32_t>(counter.size());
    if (!writer.write_byte(static_cast<std::uint8_t>(payload_size >> 24U)) ||
        !writer.write_byte(static_cast<std::uint8_t>(payload_size >> 16U)) ||
        !writer.write_byte(static_cast<std::uint8_t>(payload_size >> 8U)) ||
        !writer.write_byte(static_cast<std::uint8_t>(payload_size)) || !write_payload(writer, trace, client) ||
        writer.remaining() != 0) {
        return std::unexpected(EncodeError::InvalidTrace);
    }
    result.commit(frame_size);
    return result;
}

std::expected<mem::IoBuf, EncodeError> encode_pt1(const MessageTraceData &trace,
                                                  const ClientEncodeContext &client) noexcept {
    CountingWriter counter;
    if (!write_pt1_payload(counter, trace, client)) {
        return std::unexpected(counter.overflowed() ? EncodeError::SizeOverflow : EncodeError::InvalidTrace);
    }
    if (counter.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(EncodeError::SizeOverflow);
    }
    std::size_t frame_size = 0;
    if (!checked_add(kFramePrefixSize, counter.size(), frame_size)) {
        return std::unexpected(EncodeError::SizeOverflow);
    }
    mem::IoBuf result = mem::IoBuf::allocate(frame_size);
    if (!result) {
        return std::unexpected(EncodeError::NoMemory);
    }
    FixedWriter writer(result.writable_data(), frame_size);
    const auto payload_size = static_cast<std::uint32_t>(counter.size());
    if (!writer.write_byte(static_cast<std::uint8_t>(payload_size >> 24U)) ||
        !writer.write_byte(static_cast<std::uint8_t>(payload_size >> 16U)) ||
        !writer.write_byte(static_cast<std::uint8_t>(payload_size >> 8U)) ||
        !writer.write_byte(static_cast<std::uint8_t>(payload_size)) || !write_pt1_payload(writer, trace, client) ||
        writer.remaining() != 0) {
        return std::unexpected(EncodeError::InvalidTrace);
    }
    result.commit(frame_size);
    return result;
}

std::expected<mem::IoBuf, EncodeError>
encode_message_tree(const MessageTraceData &trace, const ClientEncodeContext &client, CatEncoderType encoder) noexcept {
    return encoder == CatEncoderType::Pt1 ? encode_pt1(trace, client) : encode_nt1(trace, client);
}

} // namespace fiber::cat::detail
