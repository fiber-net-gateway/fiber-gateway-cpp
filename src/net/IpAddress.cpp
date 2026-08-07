#include <fiber/net/IpAddress.h>

#include <arpa/inet.h>
#include <cstring>

namespace fiber::net {

IpAddress IpAddress::v4(std::array<std::uint8_t, kV4Size> bytes) noexcept {
    IpAddress out{};
    out.family_ = IpFamily::V4;
    std::memcpy(out.bytes_.data(), bytes.data(), bytes.size());
    return out;
}

IpAddress IpAddress::v6(std::array<std::uint8_t, kV6Size> bytes, std::uint32_t scope_id) noexcept {
    IpAddress out{};
    out.family_ = IpFamily::V6;
    out.bytes_ = bytes;
    out.scope_id_ = scope_id;
    return out;
}

IpAddress IpAddress::any_v4() noexcept { return v4({0, 0, 0, 0}); }

IpAddress IpAddress::any_v6() noexcept {
    std::array<std::uint8_t, kV6Size> bytes{};
    return v6(bytes);
}

IpAddress IpAddress::loopback_v4() noexcept { return v4({127, 0, 0, 1}); }

IpAddress IpAddress::loopback_v6() noexcept {
    std::array<std::uint8_t, kV6Size> bytes{};
    bytes[15] = 1;
    return v6(bytes);
}

bool IpAddress::is_loopback() const noexcept {
    if (is_v4()) {
        return bytes_[0] == 127;
    }
    if (is_v6()) {
        for (std::size_t i = 0; i + 1 < bytes_.size(); ++i) {
            if (bytes_[i] != 0) {
                return false;
            }
        }
        return bytes_[15] == 1;
    }
    return false;
}

bool IpAddress::is_unspecified() const noexcept {
    if (is_v4()) {
        return bytes_[0] == 0 && bytes_[1] == 0 && bytes_[2] == 0 && bytes_[3] == 0;
    }
    if (is_v6()) {
        for (auto byte: bytes_) {
            if (byte != 0) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool IpAddress::is_multicast() const noexcept {
    if (is_v4()) {
        return (bytes_[0] & 0xF0) == 0xE0;
    }
    if (is_v6()) {
        return bytes_[0] == 0xFF;
    }
    return false;
}

bool IpAddress::parse(std::string_view text, IpAddress &out) noexcept {
    if (text.empty()) {
        return false;
    }
    if (text.size() >= 2 && text.front() == '[' && text.back() == ']') {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    if (text.empty() || text.size() >= INET6_ADDRSTRLEN) {
        return false;
    }

    char input[INET6_ADDRSTRLEN]{};
    std::memcpy(input, text.data(), text.size());

    std::array<std::uint8_t, kV4Size> v4_bytes{};
    if (::inet_pton(AF_INET, input, v4_bytes.data()) == 1) {
        out = IpAddress::v4(v4_bytes);
        return true;
    }

    std::array<std::uint8_t, kV6Size> v6_bytes{};
    if (::inet_pton(AF_INET6, input, v6_bytes.data()) == 1) {
        out = v6(v6_bytes);
        return true;
    }
    return false;
}

std::string IpAddress::to_string() const {
    char buffer[INET6_ADDRSTRLEN]{};
    const void *src = nullptr;
    int family = AF_INET;
    if (is_v4()) {
        src = bytes_.data();
        family = AF_INET;
    } else if (is_v6()) {
        src = bytes_.data();
        family = AF_INET6;
    } else {
        return {};
    }
    const char *result = ::inet_ntop(family, src, buffer, sizeof(buffer));
    if (!result) {
        return {};
    }
    return std::string(result);
}

} // namespace fiber::net
