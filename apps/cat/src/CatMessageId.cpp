#include "CatMessageId.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <unistd.h>

#include <net/IpAddress.h>

namespace fiber::cat::detail {

namespace {

inline constexpr char kHexDigits[] = "0123456789abcdef";

std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::uint64_t process_sequence_seed() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return splitmix64(static_cast<std::uint64_t>(ticks) ^ (static_cast<std::uint64_t>(::getpid()) << 32U));
}

bool valid_domain(std::string_view domain) noexcept {
    return !domain.empty() && domain.size() <= 256 &&
           std::all_of(domain.begin(), domain.end(), [](unsigned char byte) { return byte >= 0x21 && byte <= 0x7e; });
}

bool append(std::array<char, kGeneratedMessageIdCapacity> &storage, std::size_t &size,
            std::string_view value) noexcept {
    if (value.size() > storage.size() - size) {
        return false;
    }
    std::copy(value.begin(), value.end(), storage.data() + size);
    size += value.size();
    return true;
}

bool append_number(std::array<char, kGeneratedMessageIdCapacity> &storage, std::size_t &size,
                   std::uint64_t value) noexcept {
    auto result = std::to_chars(storage.data() + size, storage.data() + storage.size(), value);
    if (result.ec != std::errc{}) {
        return false;
    }
    size = static_cast<std::size_t>(result.ptr - storage.data());
    return true;
}

} // namespace

MessageIdGenerator::MessageIdGenerator(std::string_view ip, std::uint64_t initial_sequence) noexcept :
    sequence_(initial_sequence == 0 ? process_sequence_seed() : initial_sequence) {
    net::IpAddress address;
    if (!net::IpAddress::parse(ip, address)) {
        constexpr std::string_view fallback = "00000000";
        std::copy(fallback.begin(), fallback.end(), ip_hex_.begin());
        ip_hex_size_ = fallback.size();
        return;
    }
    const std::uint8_t *bytes = address.data();
    for (std::size_t index = 0; index < address.byte_size(); ++index) {
        ip_hex_[ip_hex_size_++] = kHexDigits[bytes[index] >> 4U];
        ip_hex_[ip_hex_size_++] = kHexDigits[bytes[index] & 0x0fU];
    }
}

std::expected<GeneratedMessageId, RecordError>
MessageIdGenerator::next(std::string_view domain, std::chrono::system_clock::time_point wall_now) noexcept {
    if (!valid_domain(domain)) {
        return std::unexpected(RecordError::InvalidContext);
    }
    const std::uint64_t sequence = sequence_.fetch_add(1, std::memory_order_relaxed);
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(RecordError::IdGenerationFailed);
    }
    const auto wall_millis = std::chrono::duration_cast<std::chrono::milliseconds>(wall_now.time_since_epoch()).count();
    if (wall_millis < 0) {
        return std::unexpected(RecordError::IdGenerationFailed);
    }
    const auto hour = static_cast<std::uint64_t>(wall_millis) / (60U * 60U * 1000U);

    GeneratedMessageId result;
    if (!append(result.storage, result.size, domain) || !append(result.storage, result.size, "-") ||
        !append(result.storage, result.size, {ip_hex_.data(), ip_hex_size_}) ||
        !append(result.storage, result.size, "-") || !append_number(result.storage, result.size, hour) ||
        !append(result.storage, result.size, "-") || !append_number(result.storage, result.size, sequence + 1)) {
        return std::unexpected(RecordError::IdGenerationFailed);
    }
    return result;
}

} // namespace fiber::cat::detail
