#ifndef FIBER_CAT_ENCODER_H
#define FIBER_CAT_ENCODER_H

#include <cstdint>
#include <expected>
#include <string_view>

#include <common/mem/IoBuf.h>

namespace fiber::cat::detail {

struct MessageTraceData;

struct ClientEncodeContext {
    std::string_view app_key;
    std::string_view hostname;
    std::string_view ip;
    std::string_view thread_group_name;
    std::string_view thread_id;
    std::string_view thread_name;
};

enum class EncodeError : std::uint8_t {
    InvalidTrace,
    SizeOverflow,
    NoMemory,
};

[[nodiscard]] std::expected<mem::IoBuf, EncodeError> encode_nt1(const MessageTraceData &trace,
                                                                const ClientEncodeContext &client) noexcept;

} // namespace fiber::cat::detail

#endif // FIBER_CAT_ENCODER_H
