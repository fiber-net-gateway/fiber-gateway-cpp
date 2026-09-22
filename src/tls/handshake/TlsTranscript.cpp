#include "TlsTranscript.h"

#include <cstdlib>
#include <cstring>

#include <fiber/tls/handshake/TlsHandshakeMessage.h>

namespace fiber::tls {

bool TlsTranscript13::init(TlsHashAlgorithm hash) noexcept {
    if (!hash_.init(hash)) {
        return false;
    }
    algo_ = hash;
    inited_ = true;
    return true;
}

bool TlsTranscript13::update(std::span<const std::uint8_t> message) noexcept {
    return inited_ && hash_.update(message);
}

bool TlsTranscript13::snapshot_digest(std::span<std::uint8_t> out) const noexcept {
    if (!inited_ || out.size() != tls_hash_len(algo_)) {
        return false;
    }
    TlsHash snapshot = hash_; // final() is destructive; copy keeps the running state
    return snapshot.final(out);
}

TlsTranscript13 TlsTranscript13::fork() const noexcept { return *this; }

bool TlsTranscript13::restart_message_hash() noexcept {
    if (!inited_) {
        return false;
    }
    std::array<std::uint8_t, 64> digest{}; // EVP_MAX_MD_SIZE
    if (!hash_.final(digest)) {
        return false;
    }
    const std::size_t len = tls_hash_len(algo_);
    if (!hash_.init(algo_)) {
        return false;
    }
    const std::uint8_t header[kTlsHandshakeHeaderSize] = {static_cast<std::uint8_t>(TlsHandshakeType::MessageHash),
                                                          0x00, 0x00, static_cast<std::uint8_t>(len)};
    return hash_.update(header) && hash_.update({digest.data(), len});
}

TlsTranscript12::TlsTranscript12(TlsTranscript12 &&other) noexcept :
    hash_(other.hash_), algo_(other.algo_), buf_(other.buf_), buf_cap_(other.buf_cap_), buf_len_(other.buf_len_),
    inited_(other.inited_) {
    other.buf_ = nullptr;
    other.buf_cap_ = 0;
    other.buf_len_ = 0;
}

TlsTranscript12 &TlsTranscript12::operator=(TlsTranscript12 &&other) noexcept {
    if (this != &other) {
        release();
        hash_ = other.hash_;
        algo_ = other.algo_;
        buf_ = other.buf_;
        buf_cap_ = other.buf_cap_;
        buf_len_ = other.buf_len_;
        inited_ = other.inited_;
        other.buf_ = nullptr;
        other.buf_cap_ = 0;
        other.buf_len_ = 0;
    }
    return *this;
}

TlsTranscript12::~TlsTranscript12() noexcept { release(); }

void TlsTranscript12::release() noexcept {
    if (buf_ != nullptr) {
        tls_secure_wipe(buf_, buf_len_);
        std::free(buf_);
        buf_ = nullptr;
        buf_cap_ = 0;
        buf_len_ = 0;
    }
}

bool TlsTranscript12::reserve(std::size_t needed) noexcept {
    if (needed <= buf_cap_) {
        return true;
    }
    if (needed > kMaxBuffer) {
        return false;
    }
    std::size_t cap = buf_cap_ == 0 ? kInitialBuffer : buf_cap_;
    while (cap < needed) {
        const std::size_t doubled = cap * 2;
        if (doubled < cap || doubled > kMaxBuffer) { // overflow / clamp to the cap
            cap = kMaxBuffer;
            break;
        }
        cap = doubled;
    }
    void *grown = std::realloc(buf_, cap); // realloc(nullptr, n) == malloc(n)
    if (grown == nullptr) {
        return false;
    }
    buf_ = static_cast<std::uint8_t *>(grown);
    buf_cap_ = cap;
    return true;
}

bool TlsTranscript12::init(TlsHashAlgorithm hash) noexcept {
    if (!hash_.init(hash)) {
        return false;
    }
    algo_ = hash;
    inited_ = true;
    return true;
}

bool TlsTranscript12::update(std::span<const std::uint8_t> message) noexcept {
    if (!inited_ || !hash_.update(message)) {
        return false;
    }
    if (!reserve(buf_len_ + message.size())) {
        return false;
    }
    std::memcpy(buf_ + buf_len_, message.data(), message.size());
    buf_len_ += message.size();
    return true;
}

bool TlsTranscript12::snapshot_digest(std::span<std::uint8_t> out) const noexcept {
    if (!inited_ || out.size() != tls_hash_len(algo_)) {
        return false;
    }
    TlsHash snapshot = hash_;
    return snapshot.final(out);
}

} // namespace fiber::tls
