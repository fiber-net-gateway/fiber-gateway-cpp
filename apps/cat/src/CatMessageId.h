#ifndef FIBER_CAT_MESSAGE_ID_H
#define FIBER_CAT_MESSAGE_ID_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

#include <fiber/cat/Message.h>

namespace fiber::cat::detail {

inline constexpr std::size_t kGeneratedMessageIdCapacity = 1024;

struct GeneratedMessageId {
    std::array<char, kGeneratedMessageIdCapacity> storage{};
    std::size_t size = 0;

    [[nodiscard]] std::string_view view() const noexcept { return {storage.data(), size}; }
};

class MessageIdGenerator {
public:
    MessageIdGenerator(std::string_view ip, std::uint64_t initial_sequence = 0) noexcept;

    [[nodiscard]] std::expected<GeneratedMessageId, RecordError>
    next(std::string_view domain, std::chrono::system_clock::time_point wall_now) noexcept;

private:
    std::array<char, 32> ip_hex_{};
    std::size_t ip_hex_size_ = 0;
    std::atomic<std::uint64_t> sequence_{0};
};

} // namespace fiber::cat::detail

#endif // FIBER_CAT_MESSAGE_ID_H
