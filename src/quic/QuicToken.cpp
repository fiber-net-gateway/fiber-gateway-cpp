#include <fiber/quic/QuicToken.h>

#include <algorithm>
#include <cstring>
#include <expected>

#include <openssl/aead.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

namespace fiber::quic {

namespace {

[[nodiscard]] bool address_hash(const net::SocketAddress &peer, bool include_port, std::uint8_t *out) noexcept {
    if (out == nullptr) {
        return false;
    }
    if (!include_port) {
        if (peer.ip().is_v4()) {
            SHA1(peer.ip().v4_bytes().data(), peer.ip().v4_bytes().size(), out);
            return true;
        }
        if (peer.ip().is_v6()) {
            SHA1(peer.ip().v6_bytes().data(), peer.ip().v6_bytes().size(), out);
            return true;
        }
        return false;
    }

    sockaddr_storage storage{};
    socklen_t len = 0;
    if (!peer.to_sockaddr(storage, len)) {
        return false;
    }
    SHA1(reinterpret_cast<const std::uint8_t *>(&storage), static_cast<std::size_t>(len), out);
    return true;
}

void write_be64(std::uint8_t *out, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        out[i] = static_cast<std::uint8_t>(value >> ((sizeof(value) - 1 - i) * 8U));
    }
}

[[nodiscard]] std::uint64_t read_be64(const std::uint8_t *in) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        value = (value << 8U) | in[i];
    }
    return value;
}

[[nodiscard]] common::IoResult<void> seal_token(const std::array<std::uint8_t, kQuicAddressValidationKeyLength> &key,
                                                const std::uint8_t *plaintext, std::size_t plaintext_len,
                                                QuicAddressToken &out) noexcept {
    if ((plaintext == nullptr && plaintext_len != 0) || plaintext_len > kQuicAddressTokenPlaintextMaxLength) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (RAND_bytes(out.bytes.data(), kQuicAddressTokenIvLength) != 1) {
        return std::unexpected(common::IoErr::Invalid);
    }

    EVP_AEAD_CTX ctx;
    EVP_AEAD_CTX_zero(&ctx);
    if (!EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), key.data(), key.size(), kAeadTagLength, nullptr)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t sealed_len = 0;
    const std::size_t sealed_cap = out.bytes.size() - kQuicAddressTokenIvLength;
    const int ok = EVP_AEAD_CTX_seal(&ctx, out.bytes.data() + kQuicAddressTokenIvLength, &sealed_len, sealed_cap,
                                     out.bytes.data(), kQuicAddressTokenIvLength, plaintext, plaintext_len, nullptr, 0);
    EVP_AEAD_CTX_cleanup(&ctx);
    if (!ok) {
        return std::unexpected(common::IoErr::Invalid);
    }

    out.len = kQuicAddressTokenIvLength + sealed_len;
    return {};
}

[[nodiscard]] common::IoResult<std::size_t>
open_token(const std::array<std::uint8_t, kQuicAddressValidationKeyLength> &key, QuicSlice token,
           std::uint8_t (&plaintext)[kQuicAddressTokenPlaintextMaxLength]) noexcept {
    if (token.data == nullptr || token.len < kQuicAddressTokenIvLength + kAeadTagLength ||
        token.len > kQuicAddressTokenMaxLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    EVP_AEAD_CTX ctx;
    EVP_AEAD_CTX_zero(&ctx);
    if (!EVP_AEAD_CTX_init(&ctx, EVP_aead_aes_256_gcm(), key.data(), key.size(), kAeadTagLength, nullptr)) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::size_t plaintext_len = 0;
    const int ok = EVP_AEAD_CTX_open(&ctx, plaintext, &plaintext_len, sizeof(plaintext), token.data,
                                     kQuicAddressTokenIvLength, token.data + kQuicAddressTokenIvLength,
                                     token.len - kQuicAddressTokenIvLength, nullptr, 0);
    EVP_AEAD_CTX_cleanup(&ctx);
    if (!ok) {
        return std::unexpected(common::IoErr::Invalid);
    }
    return plaintext_len;
}

} // namespace

common::IoResult<QuicAddressToken>
quic_create_address_token(const std::array<std::uint8_t, kQuicAddressValidationKeyLength> &key,
                          const net::SocketAddress &peer, std::uint64_t expires_unix_seconds, QuicAddressTokenKind kind,
                          const QuicConnectionId *original_dcid) noexcept {
    if (kind == QuicAddressTokenKind::Retry && (original_dcid == nullptr || original_dcid->empty())) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (original_dcid != nullptr && original_dcid->size() > kMaxConnectionIdLength) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint8_t plaintext[kQuicAddressTokenPlaintextMaxLength]{};
    std::size_t offset = 0;
    const bool include_port = kind == QuicAddressTokenKind::Retry;
    if (!address_hash(peer, include_port, plaintext + offset)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    offset += kQuicAddressTokenHashLength;

    write_be64(plaintext + offset, expires_unix_seconds);
    offset += sizeof(std::uint64_t);
    plaintext[offset++] = static_cast<std::uint8_t>(kind);

    const std::uint8_t odcid_len = original_dcid == nullptr ? 0 : static_cast<std::uint8_t>(original_dcid->size());
    plaintext[offset++] = odcid_len;
    if (odcid_len != 0) {
        std::memcpy(plaintext + offset, original_dcid->data(), odcid_len);
        offset += odcid_len;
    }

    QuicAddressToken token{};
    auto sealed = seal_token(key, plaintext, offset, token);
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    return token;
}

common::IoResult<QuicAddressTokenValidation>
quic_validate_address_token(const std::array<std::uint8_t, kQuicAddressValidationKeyLength> &key,
                            const net::SocketAddress &peer, std::uint64_t now_unix_seconds, QuicSlice token) noexcept {
    QuicAddressTokenValidation result{};
    std::uint8_t plaintext[kQuicAddressTokenPlaintextMaxLength]{};
    auto opened = open_token(key, token, plaintext);
    if (!opened) {
        result.status = QuicAddressTokenValidationStatus::Garbage;
        return result;
    }

    const std::size_t min_len = kQuicAddressTokenHashLength + sizeof(std::uint64_t) + 1 + 1;
    if (*opened < min_len) {
        result.status = QuicAddressTokenValidationStatus::Garbage;
        return result;
    }

    std::size_t offset = kQuicAddressTokenHashLength;
    const std::uint64_t expires = read_be64(plaintext + offset);
    offset += sizeof(std::uint64_t);

    const auto kind = static_cast<QuicAddressTokenKind>(plaintext[offset++]);
    if (kind != QuicAddressTokenKind::NewToken && kind != QuicAddressTokenKind::Retry) {
        result.status = QuicAddressTokenValidationStatus::Garbage;
        return result;
    }
    result.kind = kind;

    const std::uint8_t odcid_len = plaintext[offset++];
    if (odcid_len > kMaxConnectionIdLength || *opened - offset != odcid_len) {
        result.status = QuicAddressTokenValidationStatus::Garbage;
        return result;
    }
    if (kind == QuicAddressTokenKind::Retry && odcid_len == 0) {
        result.status = QuicAddressTokenValidationStatus::Garbage;
        return result;
    }
    if (odcid_len != 0) {
        auto cid = QuicConnectionId::from_bytes(plaintext + offset, odcid_len);
        if (!cid) {
            result.status = QuicAddressTokenValidationStatus::Garbage;
            return result;
        }
        result.original_destination_connection_id = *cid;
    }

    std::uint8_t expected_hash[kQuicAddressTokenHashLength]{};
    const bool include_port = kind == QuicAddressTokenKind::Retry;
    if (!address_hash(peer, include_port, expected_hash)) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (std::memcmp(plaintext, expected_hash, sizeof(expected_hash)) != 0) {
        result.status = QuicAddressTokenValidationStatus::Invalid;
        return result;
    }

    if (now_unix_seconds > expires) {
        result.status = QuicAddressTokenValidationStatus::Expired;
        return result;
    }

    result.status = QuicAddressTokenValidationStatus::Valid;
    return result;
}

std::uint64_t quic_unix_seconds_now() noexcept {
    return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
}

} // namespace fiber::quic
