#ifndef FIBER_CAT_MESSAGE_H
#define FIBER_CAT_MESSAGE_H

#include <cstddef>
#include <cstdint>

namespace fiber::cat {

namespace detail {
struct EventData;
struct TransactionData;
} // namespace detail

enum class MessageKind : std::uint8_t {
    Transaction,
    Event,
};

enum class RecordError : std::uint8_t {
    None,
    InvalidArgument,
    WrongEventLoop,
    Completed,
    LimitExceeded,
    NoMemory,
    WrongMetricKind,
};

struct RecordLimits {
    std::size_t max_messages = 2000;
    std::size_t max_children_per_transaction = 2048;
    std::size_t max_type_bytes = 256;
    std::size_t max_name_bytes = 4096;
    std::size_t max_status_bytes = 256;
    std::size_t max_data_bytes_per_message = 64 * 1024;
    std::size_t max_context_entries = 32;
    std::size_t max_context_key_bytes = 128;
    std::size_t max_context_value_bytes = 4096;
    std::size_t max_context_bytes = 16 * 1024;
    std::size_t max_tree_bytes = 1024 * 1024;
};

} // namespace fiber::cat

#endif // FIBER_CAT_MESSAGE_H
