#include <fiber/dns/detail/DnsQuerySecurity.h>

#include <array>
#include <string_view>

#include <fiber/dns/DnsName.h>

namespace fiber::dns::detail {

namespace {

constexpr std::size_t kDnsHeaderSize = 12;
constexpr std::size_t kMaxDecodedNameSize = 255;

struct ParsedQuestion {
    std::string_view name{};
    std::uint16_t type = 0;
    std::uint16_t dns_class = 0;
};

std::uint16_t read_be16(const std::uint8_t *data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

std::uint8_t ascii_lower(std::uint8_t ch) noexcept {
    if (ch >= static_cast<std::uint8_t>('A') && ch <= static_cast<std::uint8_t>('Z')) {
        return static_cast<std::uint8_t>(ch + ('a' - 'A'));
    }
    return ch;
}

bool is_ascii_letter(std::uint8_t ch) noexcept {
    const std::uint8_t lower = ascii_lower(ch);
    return lower >= static_cast<std::uint8_t>('a') && lower <= static_cast<std::uint8_t>('z');
}

bool names_equal(std::string_view a, std::string_view b, bool exact_case) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto left = static_cast<std::uint8_t>(a[i]);
        const auto right = static_cast<std::uint8_t>(b[i]);
        if (exact_case ? left != right : ascii_lower(left) != ascii_lower(right)) {
            return false;
        }
    }
    return true;
}

bool parse_question(const std::uint8_t *packet, std::size_t packet_len, std::array<char, kMaxDecodedNameSize> &storage,
                    ParsedQuestion &out) noexcept {
    auto decoded = decode_name(packet, packet_len, kDnsHeaderSize, storage.data(), storage.size());
    if (!decoded || decoded->next_offset > packet_len || packet_len - decoded->next_offset < 4) {
        return false;
    }

    out.name = decoded->name;
    out.type = read_be16(packet + decoded->next_offset);
    out.dns_class = read_be16(packet + decoded->next_offset + 2);
    return true;
}

} // namespace

common::IoResult<std::uint16_t> select_query_id(const std::uint16_t *id_to_slot, std::uint16_t invalid_mapping,
                                                std::uint16_t start, std::uint16_t stride) noexcept {
    if (id_to_slot == nullptr) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint16_t candidate = start;
    const std::uint16_t odd_stride = static_cast<std::uint16_t>(stride | 1U);
    for (std::size_t i = 0; i < kDnsQueryIdCount; ++i) {
        if (id_to_slot[candidate] == invalid_mapping) {
            return candidate;
        }
        candidate = static_cast<std::uint16_t>(candidate + odd_stride);
    }
    return std::unexpected(common::IoErr::Busy);
}

common::IoErr apply_query_name_0x20(std::uint8_t *packet, std::size_t packet_len,
                                    std::span<const std::uint8_t, kDns0x20RandomBytes> random_bits) noexcept {
    if (packet == nullptr || packet_len < kDnsHeaderSize || read_be16(packet + 4) != 1) {
        return common::IoErr::Invalid;
    }

    std::size_t offset = kDnsHeaderSize;
    std::size_t bit_index = 0;
    while (true) {
        if (offset >= packet_len) {
            return common::IoErr::Invalid;
        }
        const std::uint8_t label_len = packet[offset++];
        if (label_len == 0) {
            return packet_len - offset >= 4 ? common::IoErr::None : common::IoErr::Invalid;
        }
        if (label_len > 63 || static_cast<std::size_t>(label_len) > packet_len - offset) {
            return common::IoErr::Invalid;
        }

        for (std::size_t i = 0; i < label_len; ++i) {
            std::uint8_t &ch = packet[offset + i];
            if (!is_ascii_letter(ch)) {
                continue;
            }
            if (bit_index >= random_bits.size() * 8) {
                return common::IoErr::Invalid;
            }
            const bool uppercase = ((random_bits[bit_index / 8] >> (bit_index % 8)) & 1U) != 0;
            ch = ascii_lower(ch);
            if (uppercase) {
                ch = static_cast<std::uint8_t>(ch - ('a' - 'A'));
            }
            ++bit_index;
        }
        offset += label_len;
    }
}

bool response_matches_query(const std::uint8_t *request, std::size_t request_len, const std::uint8_t *response,
                            std::size_t response_len, bool exact_name_case) noexcept {
    if (request == nullptr || response == nullptr || request_len < kDnsHeaderSize || response_len < kDnsHeaderSize) {
        return false;
    }
    if (read_be16(request) != read_be16(response)) {
        return false;
    }

    const std::uint16_t request_flags = read_be16(request + 2);
    const std::uint16_t response_flags = read_be16(response + 2);
    constexpr std::uint16_t kResponseFlag = 0x8000U;
    constexpr std::uint16_t kOpcodeMask = 0x7800U;
    if ((request_flags & kResponseFlag) != 0 || (response_flags & kResponseFlag) == 0 ||
        (request_flags & kOpcodeMask) != 0 || (response_flags & kOpcodeMask) != 0 || read_be16(request + 4) != 1 ||
        read_be16(response + 4) != 1) {
        return false;
    }

    std::array<char, kMaxDecodedNameSize> request_name{};
    std::array<char, kMaxDecodedNameSize> response_name{};
    ParsedQuestion request_question;
    ParsedQuestion response_question;
    if (!parse_question(request, request_len, request_name, request_question) ||
        !parse_question(response, response_len, response_name, response_question)) {
        return false;
    }

    return request_question.type == response_question.type &&
           request_question.dns_class == response_question.dns_class &&
           names_equal(request_question.name, response_question.name, exact_name_case);
}

} // namespace fiber::dns::detail
