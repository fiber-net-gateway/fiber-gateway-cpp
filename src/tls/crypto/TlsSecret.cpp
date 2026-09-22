#include <fiber/tls/crypto/TlsSecret.h>

#include <cstring>

#include <fiber/common/Assert.h>

#include "TlsCryptoPrimitives.h"

namespace fiber::tls {

TlsSecret::TlsSecret(TlsSecret &&other) noexcept {
    std::memcpy(buf_.data(), other.buf_.data(), other.len_);
    len_ = other.len_;
    other.wipe();
}

TlsSecret &TlsSecret::operator=(TlsSecret &&other) noexcept {
    if (this != &other) {
        wipe();
        std::memcpy(buf_.data(), other.buf_.data(), other.len_);
        len_ = other.len_;
        other.wipe();
    }
    return *this;
}

void TlsSecret::wipe() noexcept {
    tls_secure_wipe(buf_.data(), buf_.size());
    len_ = 0;
}

TlsSecret TlsSecret::from_bytes(std::span<const std::uint8_t> src) noexcept {
    FIBER_ASSERT(src.size() <= kMaxLen);
    TlsSecret out;
    std::memcpy(out.buf_.data(), src.data(), src.size());
    out.len_ = static_cast<std::uint8_t>(src.size());
    return out;
}

} // namespace fiber::tls
