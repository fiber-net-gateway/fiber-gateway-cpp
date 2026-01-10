#include "UnixAddress.h"

#include <cstddef>
#include <cstring>
#include <sys/un.h>

namespace fiber::net {

namespace {

constexpr size_t kSunPathOffset = offsetof(sockaddr_un, sun_path);

} // namespace

UnixAddress::UnixAddress(UnixAddressKind kind, std::string value)
    : kind_(kind), value_(std::move(value)) {
}

UnixAddress UnixAddress::filesystem(std::string path) {
    return UnixAddress(UnixAddressKind::Filesystem, std::move(path));
}

UnixAddress UnixAddress::abstract(std::string bytes) {
    return UnixAddress(UnixAddressKind::Abstract, std::move(bytes));
}

UnixAddress UnixAddress::unnamed() {
    return UnixAddress(UnixAddressKind::Unnamed, {});
}

UnixAddressKind UnixAddress::kind() const noexcept {
    return kind_;
}

const std::string &UnixAddress::path() const noexcept {
    return value_;
}

const std::string &UnixAddress::bytes() const noexcept {
    return value_;
}

bool UnixAddress::to_sockaddr(sockaddr_storage &out, socklen_t &len) const noexcept {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (kind_ == UnixAddressKind::Unnamed) {
        len = static_cast<socklen_t>(kSunPathOffset);
        std::memcpy(&out, &addr, len);
        return true;
    }
    const size_t max_len = sizeof(addr.sun_path);
    if (kind_ == UnixAddressKind::Filesystem) {
        if (value_.empty() || value_.size() >= max_len) {
            return false;
        }
        std::memcpy(addr.sun_path, value_.data(), value_.size());
        addr.sun_path[value_.size()] = '\0';
        len = static_cast<socklen_t>(kSunPathOffset + value_.size() + 1);
        std::memcpy(&out, &addr, len);
        return true;
    }
    if (kind_ == UnixAddressKind::Abstract) {
        if (value_.size() >= max_len) {
            return false;
        }
        addr.sun_path[0] = '\0';
        if (!value_.empty()) {
            std::memcpy(addr.sun_path + 1, value_.data(), value_.size());
        }
        len = static_cast<socklen_t>(kSunPathOffset + 1 + value_.size());
        std::memcpy(&out, &addr, len);
        return true;
    }
    return false;
}

bool UnixAddress::from_sockaddr(const sockaddr *addr, socklen_t len, UnixAddress &out) noexcept {
    if (!addr || addr->sa_family != AF_UNIX) {
        return false;
    }
    if (len < kSunPathOffset) {
        return false;
    }
    const auto *un = reinterpret_cast<const sockaddr_un *>(addr);
    size_t path_len = len > kSunPathOffset ? static_cast<size_t>(len - kSunPathOffset) : 0;
    if (path_len > sizeof(un->sun_path)) {
        path_len = sizeof(un->sun_path);
    }
    if (path_len == 0) {
        out = UnixAddress::unnamed();
        return true;
    }
    if (un->sun_path[0] == '\0') {
        if (path_len <= 1) {
            out = UnixAddress::abstract({});
            return true;
        }
        std::string bytes(un->sun_path + 1, un->sun_path + path_len);
        out = UnixAddress::abstract(std::move(bytes));
        return true;
    }
    size_t max_len = path_len;
    size_t actual_len = strnlen(un->sun_path, max_len);
    std::string path(un->sun_path, un->sun_path + actual_len);
    out = UnixAddress::filesystem(std::move(path));
    return true;
}

std::string UnixAddress::to_string() const {
    if (kind_ == UnixAddressKind::Filesystem) {
        return value_;
    }
    if (kind_ == UnixAddressKind::Abstract) {
        std::string out;
        out.reserve(value_.size() + 1);
        out.push_back('@');
        out.append(value_);
        return out;
    }
    return "<unnamed>";
}

} // namespace fiber::net
