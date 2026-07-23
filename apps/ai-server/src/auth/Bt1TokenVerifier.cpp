#include "Bt1TokenVerifier.h"

#include "../config/LlmConfigSnapshot.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

#include <openssl/hmac.h>
#include <openssl/mem.h>

#include <common/json/Utf.h>

namespace fiber::ai_server {
namespace {

constexpr std::string_view kBt1Version = "BT1";
constexpr std::size_t kBt1RandomLength = 22;
constexpr std::size_t kBt1MacTextLength = 43;
constexpr std::size_t kBt1MacLength = 32;

Bt1AuthError invalid(Bt1AuthFailureReason reason) noexcept {
    return {
            .kind = Bt1AuthErrorKind::InvalidToken,
            .reason = reason,
    };
}

Bt1AuthError expired() noexcept {
    return {
            .kind = Bt1AuthErrorKind::ExpiredToken,
            .reason = Bt1AuthFailureReason::Expired,
    };
}

Bt1AuthError internal(Bt1AuthFailureReason reason) noexcept {
    return {
            .kind = Bt1AuthErrorKind::InternalError,
            .reason = reason,
    };
}

bool valid_kid(std::string_view kid) noexcept {
    if (kid.empty() || kid.size() > kBt1MaxKidLength) {
        return false;
    }
    for (const unsigned char ch: kid) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
            ch == '-') {
            continue;
        }
        return false;
    }
    return true;
}

int base64url_digit(unsigned char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '-') {
        return 62;
    }
    if (ch == '_') {
        return 63;
    }
    return -1;
}

bool valid_base64url_no_pad(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const unsigned char ch: text) {
        if (base64url_digit(ch) < 0) {
            return false;
        }
    }
    return true;
}

bool decode_base64url_no_pad(std::string_view text, std::span<std::uint8_t> output, std::size_t &written) noexcept {
    written = 0;
    const std::size_t remainder = text.size() & 3U;
    if (remainder == 1) {
        return false;
    }
    const std::size_t decoded_size = (text.size() / 4) * 3 + (remainder == 0 ? 0 : remainder - 1);
    if (decoded_size > output.size()) {
        return false;
    }

    std::size_t input_pos = 0;
    while (input_pos + 4 <= text.size()) {
        const int a = base64url_digit(static_cast<unsigned char>(text[input_pos]));
        const int b = base64url_digit(static_cast<unsigned char>(text[input_pos + 1]));
        const int c = base64url_digit(static_cast<unsigned char>(text[input_pos + 2]));
        const int d = base64url_digit(static_cast<unsigned char>(text[input_pos + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0) {
            return false;
        }
        output[written++] = static_cast<std::uint8_t>((a << 2) | (b >> 4));
        output[written++] = static_cast<std::uint8_t>((b << 4) | (c >> 2));
        output[written++] = static_cast<std::uint8_t>((c << 6) | d);
        input_pos += 4;
    }

    if (remainder >= 2) {
        const int a = base64url_digit(static_cast<unsigned char>(text[input_pos]));
        const int b = base64url_digit(static_cast<unsigned char>(text[input_pos + 1]));
        if (a < 0 || b < 0) {
            return false;
        }
        output[written++] = static_cast<std::uint8_t>((a << 2) | (b >> 4));
        if (remainder == 3) {
            const int c = base64url_digit(static_cast<unsigned char>(text[input_pos + 2]));
            if (c < 0) {
                return false;
            }
            output[written++] = static_cast<std::uint8_t>((b << 4) | (c >> 2));
        }
    }
    return written == decoded_size;
}

bool parse_expiration(std::string_view text, std::int64_t &expiration) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return false;
    }
    std::int64_t value = 0;
    for (const unsigned char ch: text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::int64_t digit = ch - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    expiration = value;
    return true;
}

bool is_expired(std::int64_t expiration, std::int32_t clock_skew_seconds, std::int64_t now_seconds) noexcept {
    if (now_seconds <= expiration) {
        return false;
    }
    return now_seconds - expiration > clock_skew_seconds;
}

} // namespace

Bt1Principal::Bt1Principal(std::string_view username, std::string_view kid, std::int64_t expires_at) noexcept :
    username_size_(static_cast<std::uint8_t>(username.size())), kid_size_(static_cast<std::uint8_t>(kid.size())),
    expires_at_(expires_at) {
    std::memcpy(username_.data(), username.data(), username.size());
    std::memcpy(kid_.data(), kid.data(), kid.size());
}

std::expected<Bt1Principal, Bt1AuthError> verify_bt1_token(std::string_view token, const Bt1KeySnapshot &key_ring,
                                                           std::int64_t now_seconds) noexcept {
    if (token.empty()) {
        return std::unexpected(invalid(Bt1AuthFailureReason::EmptyToken));
    }
    if (token.size() > kBt1MaxTokenLength) {
        return std::unexpected(invalid(Bt1AuthFailureReason::TokenTooLong));
    }
    if (key_ring.clock_skew_seconds < 0 || key_ring.clock_skew_seconds > 300) {
        return std::unexpected(internal(Bt1AuthFailureReason::AuthConfigUnavailable));
    }

    std::array<std::size_t, 5> dots{};
    std::size_t dot_count = 0;
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] != '.') {
            continue;
        }
        if (dot_count == dots.size()) {
            return std::unexpected(invalid(Bt1AuthFailureReason::InvalidSegmentCount));
        }
        dots[dot_count++] = i;
    }
    if (dot_count != dots.size() || dots[0] == 0 || dots[1] <= dots[0] + 1 || dots[2] <= dots[1] + 1 ||
        dots[3] <= dots[2] + 1 || dots[4] <= dots[3] + 1 || dots[4] + 1 >= token.size()) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidSegmentCount));
    }
    if (token.substr(0, dots[0]) != kBt1Version) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidVersion));
    }

    const std::string_view kid = token.substr(dots[0] + 1, dots[1] - dots[0] - 1);
    if (!valid_kid(kid)) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidKid));
    }
    const Bt1Key *key = key_ring.find_key(kid);
    if (!key) {
        return std::unexpected(invalid(Bt1AuthFailureReason::UnknownKid));
    }
    if (key->secret.empty()) {
        return std::unexpected(internal(Bt1AuthFailureReason::AuthConfigUnavailable));
    }

    const std::string_view user = token.substr(dots[1] + 1, dots[2] - dots[1] - 1);
    const std::string_view expiration_text = token.substr(dots[2] + 1, dots[3] - dots[2] - 1);
    const std::string_view random = token.substr(dots[3] + 1, dots[4] - dots[3] - 1);
    const std::string_view mac_text = token.substr(dots[4] + 1);
    if (!valid_base64url_no_pad(user)) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidUserEncoding));
    }
    if (random.size() != kBt1RandomLength || !valid_base64url_no_pad(random)) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidRandomEncoding));
    }
    if (mac_text.size() != kBt1MacTextLength || !valid_base64url_no_pad(mac_text)) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidMacEncoding));
    }

    std::int64_t expiration = 0;
    if (!parse_expiration(expiration_text, expiration)) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidExpiration));
    }
    if (is_expired(expiration, key_ring.clock_skew_seconds, now_seconds)) {
        return std::unexpected(expired());
    }

    std::array<std::uint8_t, kBt1MacLength> actual_mac{};
    std::size_t actual_mac_size = 0;
    if (!decode_base64url_no_pad(mac_text, actual_mac, actual_mac_size) || actual_mac_size != actual_mac.size()) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidMacEncoding));
    }

    std::array<std::uint8_t, EVP_MAX_MD_SIZE> expected_mac{};
    unsigned int expected_mac_size = 0;
    const std::string_view signing_input = token.substr(0, dots[4]);
    if (!HMAC(EVP_sha256(), key->secret.data(), key->secret.size(),
              reinterpret_cast<const std::uint8_t *>(signing_input.data()), signing_input.size(), expected_mac.data(),
              &expected_mac_size) ||
        expected_mac_size != kBt1MacLength) {
        return std::unexpected(internal(Bt1AuthFailureReason::CryptoFailure));
    }
    if (CRYPTO_memcmp(actual_mac.data(), expected_mac.data(), kBt1MacLength) != 0) {
        return std::unexpected(invalid(Bt1AuthFailureReason::MacMismatch));
    }

    std::array<std::uint8_t, kBt1MaxUsernameBytes> username{};
    std::size_t username_size = 0;
    if (!decode_base64url_no_pad(user, username, username_size) || username_size == 0 ||
        !json::utf8_validate(reinterpret_cast<const char *>(username.data()), username_size)) {
        return std::unexpected(invalid(Bt1AuthFailureReason::InvalidUsername));
    }
    return Bt1Principal({reinterpret_cast<const char *>(username.data()), username_size}, kid, expiration);
}

std::string_view bt1_auth_error_name(Bt1AuthErrorKind kind) noexcept {
    switch (kind) {
        case Bt1AuthErrorKind::InvalidToken:
            return "invalid_token";
        case Bt1AuthErrorKind::ExpiredToken:
            return "expired_token";
        case Bt1AuthErrorKind::InternalError:
            return "token_verification_error";
    }
    return "token_verification_error";
}

std::string_view bt1_auth_error_message(Bt1AuthErrorKind kind) noexcept {
    switch (kind) {
        case Bt1AuthErrorKind::InvalidToken:
            return "invalid bearer token";
        case Bt1AuthErrorKind::ExpiredToken:
            return "expired bearer token";
        case Bt1AuthErrorKind::InternalError:
            return "bearer token verification failed";
    }
    return "bearer token verification failed";
}

} // namespace fiber::ai_server
