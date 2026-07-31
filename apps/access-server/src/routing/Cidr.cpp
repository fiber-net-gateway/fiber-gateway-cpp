#include "Cidr.h"

#include <charconv>
#include <limits>
#include <string>

namespace fiber::access_server {
namespace {

AccessConfigError cidr_error(std::string_view field, std::string_view text) {
    std::string message = "invalid cidr: ";
    message.append(text);
    return AccessConfigError{
            .code = AccessConfigErrorCode::InvalidField,
            .field = std::string(field),
            .message = std::move(message),
    };
}

bool parse_prefix(std::string_view text, std::uint32_t &prefix) noexcept {
    if (text.empty()) {
        return false;
    }
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty()) {
            return false;
        }
    }

    std::uint32_t parsed = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    prefix = parsed;
    return true;
}

bool parse_java_v4(std::string_view text, std::array<std::uint8_t, net::IpAddress::kV6Size> &bytes) noexcept {
    if (text.empty() || text.size() > 15) {
        return false;
    }

    std::uint32_t value = 0;
    std::size_t output = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            if (value == 0 && i > 0 && text[i - 1] != '.') {
                return false;
            }
            value = value * 10U + static_cast<std::uint32_t>(ch - '0');
            if (value > 255) {
                return false;
            }
        } else if (ch == '.') {
            if (output >= 3) {
                return false;
            }
            bytes[output++] = static_cast<std::uint8_t>(value);
            value = 0;
        } else {
            return false;
        }
    }
    if (output != 3 || text.back() == '.') {
        return false;
    }
    bytes[3] = static_cast<std::uint8_t>(value);
    return true;
}

bool parse_java_ip(std::string_view text, std::array<std::uint8_t, net::IpAddress::kV6Size> &bytes,
                   std::size_t &byte_size) noexcept {
    if (parse_java_v4(text, bytes)) {
        byte_size = net::IpAddress::kV4Size;
        return true;
    }

    net::IpAddress address{};
    std::string_view v6_text = text;
    if (v6_text.size() >= 2 && v6_text.front() == '[') {
        // IpUtils.parseIp unconditionally drops the first and last character
        // for a bracket-prefixed value; it does not verify the closing byte.
        v6_text = v6_text.substr(1, v6_text.size() - 2);
    }
    if (!net::IpAddress::parse(v6_text, address) || !address.is_v6()) {
        return false;
    }
    byte_size = net::IpAddress::kV6Size;
    for (std::size_t i = 0; i < byte_size; ++i) {
        bytes[i] = address.data()[i];
    }
    return true;
}

} // namespace

std::expected<Cidr, AccessConfigError> Cidr::parse(std::string_view text, std::string_view field) {
    if (text.empty()) {
        return std::unexpected(cidr_error(field, text));
    }

    const std::size_t slash = text.find('/');
    const std::string_view address_text = slash == std::string_view::npos ? text : text.substr(0, slash);

    std::array<std::uint8_t, net::IpAddress::kV6Size> address{};
    std::size_t byte_size = 0;
    if (!parse_java_ip(address_text, address, byte_size)) {
        return std::unexpected(cidr_error(field, text));
    }

    const std::uint32_t max_prefix = static_cast<std::uint32_t>(byte_size * 8);
    std::uint32_t prefix = max_prefix;
    if (slash != std::string_view::npos && (!parse_prefix(text.substr(slash + 1), prefix) || prefix > max_prefix)) {
        return std::unexpected(cidr_error(field, text));
    }

    Cidr cidr;
    cidr.byte_size_ = static_cast<std::uint8_t>(byte_size);
    cidr.prefix_length_ = static_cast<std::uint8_t>(prefix);
    for (std::size_t i = 0; i < byte_size; ++i) {
        cidr.network_[i] = address[i];
    }

    const std::size_t whole_bytes = prefix / 8;
    const std::uint32_t remaining_bits = prefix % 8;
    if (remaining_bits != 0) {
        cidr.network_[whole_bytes] &= static_cast<std::uint8_t>(0xFFU << (8U - remaining_bits));
    }
    const std::size_t zero_begin = whole_bytes + (remaining_bits == 0 ? 0 : 1);
    for (std::size_t i = zero_begin; i < byte_size; ++i) {
        cidr.network_[i] = 0;
    }
    return cidr;
}

std::expected<std::vector<Cidr>, AccessConfigError> Cidr::parse_list(std::span<const std::string_view> values,
                                                                     std::string_view field) {
    std::vector<Cidr> parsed;
    parsed.reserve(values.size());
    for (std::string_view value: values) {
        auto cidr = parse(value, field);
        if (!cidr) {
            return std::unexpected(std::move(cidr.error()));
        }
        parsed.push_back(*cidr);
    }
    if (parsed.size() < 2) {
        return parsed;
    }

    std::vector<bool> removed(parsed.size(), false);
    std::size_t removed_count = 0;
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        if (removed[i]) {
            continue;
        }
        for (std::size_t j = i + 1; j < parsed.size(); ++j) {
            if (removed[j]) {
                continue;
            }
            if (parsed[i].contains(parsed[j])) {
                removed[j] = true;
                ++removed_count;
            } else if (parsed[j].contains(parsed[i])) {
                removed[i] = true;
                ++removed_count;
                break;
            }
        }
    }

    std::vector<Cidr> result;
    result.reserve(parsed.size() - removed_count);
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        if (!removed[i]) {
            result.push_back(parsed[i]);
        }
    }
    return result;
}

bool Cidr::contains(const Cidr &other) const noexcept {
    if (byte_size_ != other.byte_size_ || prefix_length_ > other.prefix_length_) {
        return false;
    }
    return matches(other);
}

bool Cidr::matches(const Cidr &other) const noexcept {
    if (byte_size_ != other.byte_size_) {
        return false;
    }
    const std::size_t whole_bytes = prefix_length_ / 8;
    for (std::size_t i = 0; i < whole_bytes; ++i) {
        if (network_[i] != other.network_[i]) {
            return false;
        }
    }
    const std::uint32_t remaining_bits = prefix_length_ % 8;
    if (remaining_bits == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - remaining_bits));
    return (network_[whole_bytes] & mask) == (other.network_[whole_bytes] & mask);
}

bool Cidr::matches(const net::IpAddress &address) const noexcept {
    if (address.byte_size() != byte_size_) {
        return false;
    }
    const std::size_t whole_bytes = prefix_length_ / 8;
    for (std::size_t i = 0; i < whole_bytes; ++i) {
        if (network_[i] != address.data()[i]) {
            return false;
        }
    }
    const std::uint32_t remaining_bits = prefix_length_ % 8;
    if (remaining_bits == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xFFU << (8U - remaining_bits));
    return (network_[whole_bytes] & mask) == (address.data()[whole_bytes] & mask);
}

} // namespace fiber::access_server
