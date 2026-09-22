#ifndef FIBER_TLS_HANDSHAKE_TLS_TRANSCRIPT_H
#define FIBER_TLS_HANDSHAKE_TLS_TRANSCRIPT_H

// Running handshake transcript (06 §5.2). Internal to src/tls — the engine
// holds it by value, and TlsHash (SHA context) must stay behind the crypto
// adapter boundary (01 §3.1). Both classes take COMPLETE handshake messages
// (4-byte header + body) on update; snapshots are non-destructive.

#include <array>
#include <cstddef>
#include <span>

#include "../crypto/TlsCryptoPrimitives.h"

namespace fiber::tls {

// TLS 1.3 running hash. Created at the ServerHello read point with the
// negotiated suite's hash — the client feeds its retained CH1/CH2 encoded
// bytes first (while the version is undecided no transcript object exists,
// 06 §5.2). Trivially copyable: fork() is a state copy (post-handshake
// CertificateVerify forks the transcript this way; the client handshake
// itself never needs to).
class TlsTranscript13 {
public:
    TlsTranscript13() noexcept = default;
    TlsTranscript13(const TlsTranscript13 &) noexcept = default;
    TlsTranscript13 &operator=(const TlsTranscript13 &) noexcept = default;
    ~TlsTranscript13() = default;

    [[nodiscard]] bool init(TlsHashAlgorithm hash) noexcept;
    [[nodiscard]] bool update(std::span<const std::uint8_t> message) noexcept;
    // out.size() must equal tls_hash_len(hash); writes the running digest.
    [[nodiscard]] bool snapshot_digest(std::span<std::uint8_t> out) const noexcept;
    [[nodiscard]] TlsTranscript13 fork() const noexcept;
    // HelloRetryRequest restart (RFC 8446 §4.1.4/§4.4.1): this transcript's
    // current digest (the engine has fed only CH1) becomes the sole content
    // of the synthetic message_hash message
    //   254 || 0x00 0x00 <hash_len> || Transcript-Hash(CH1)
    // and the running hash restarts over it; the caller then feeds the HRR
    // and CH2 into the restarted hash. The hash algorithm is kept.
    [[nodiscard]] bool restart_message_hash() noexcept;

private:
    TlsHash hash_;
    TlsHashAlgorithm algo_ = TlsHashAlgorithm::Sha256;
    bool inited_ = false;
};

// TLS 1.2 handshake transcript: running hash PLUS the raw message bytes.
// The byte buffer is not optional — a TLS 1.2 client CertificateVerify signs
// the concatenated handshake_messages themselves (BoringSSL signs
// hs->transcript.buffer(); EVP applies the scheme digest internally, so no
// pre-digested form exists). Every snapshot point is post-ServerHello, so the
// suite hash is known before the first update. The buffer grows on demand
// (malloc/realloc; failure returns false) up to a 9 MiB cap — the 06 §2.6
// inbound bound is 4 MiB per reassembled message, so the cap covers a maximal
// two-sided flight plus the retained CH. Move-only: the engine holds it by
// value and never copies it.
class TlsTranscript12 {
public:
    static constexpr std::size_t kInitialBuffer = 16 * 1024;
    static constexpr std::size_t kMaxBuffer = 9u << 20;

    TlsTranscript12() noexcept = default;
    TlsTranscript12(const TlsTranscript12 &) noexcept = delete;
    TlsTranscript12 &operator=(const TlsTranscript12 &) noexcept = delete;
    TlsTranscript12(TlsTranscript12 &&other) noexcept;
    TlsTranscript12 &operator=(TlsTranscript12 &&other) noexcept;
    ~TlsTranscript12() noexcept;

    [[nodiscard]] bool init(TlsHashAlgorithm hash) noexcept;
    // Feeds the hash and appends to the byte buffer (the 1.2 CV signs it).
    [[nodiscard]] bool update(std::span<const std::uint8_t> message) noexcept;
    // out.size() must equal tls_hash_len(hash); writes the running digest.
    [[nodiscard]] bool snapshot_digest(std::span<std::uint8_t> out) const noexcept;
    // The raw concatenated handshake messages so far (CV signature content).
    [[nodiscard]] std::span<const std::uint8_t> buffer() const noexcept { return {buf_, buf_len_}; }

private:
    void release() noexcept;
    [[nodiscard]] bool reserve(std::size_t needed) noexcept;

    TlsHash hash_;
    TlsHashAlgorithm algo_ = TlsHashAlgorithm::Sha256;
    std::uint8_t *buf_ = nullptr; // malloc'd; wiped then freed in the destructor
    std::size_t buf_cap_ = 0;
    std::size_t buf_len_ = 0;
    bool inited_ = false;
};

} // namespace fiber::tls

#endif // FIBER_TLS_HANDSHAKE_TLS_TRANSCRIPT_H
