#include "CatEncoder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
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

template<typename Writer>
bool write_data(Writer &writer, const MessageData &message) noexcept {
    if (!write_unsigned(writer, message.data_size)) {
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
    return observed == message.data_size;
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

    if (message.kind == MessageKind::Event) {
        return writer.write_byte('E') && write_unsigned(writer, timestamp) && write_string(writer, message.type) &&
               write_string(writer, message.name) && write_string(writer, message.status) &&
               write_data(writer, message);
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

    return writer.write_byte('T') && write_string(writer, message.status) && write_data(writer, message) &&
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

} // namespace fiber::cat::detail
