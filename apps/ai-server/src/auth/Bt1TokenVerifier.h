#ifndef FIBER_AI_SERVER_BT1_TOKEN_VERIFIER_H
#define FIBER_AI_SERVER_BT1_TOKEN_VERIFIER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>

namespace fiber::ai_server {

struct Bt1KeySnapshot;

inline constexpr std::size_t kBt1MaxTokenLength = 512;
inline constexpr std::size_t kBt1MaxUsernameBytes = 64;
inline constexpr std::size_t kBt1MaxKidLength = 16;

enum class Bt1AuthErrorKind : std::uint8_t {
    InvalidToken,
    ExpiredToken,
    InternalError,
};

enum class Bt1AuthFailureReason : std::uint8_t {
    MissingCredential,
    InvalidAuthorizationScheme,
    EmptyToken,
    TokenTooLong,
    InvalidSegmentCount,
    InvalidVersion,
    InvalidKid,
    UnknownKid,
    InvalidUserEncoding,
    InvalidRandomEncoding,
    InvalidMacEncoding,
    InvalidExpiration,
    Expired,
    MacMismatch,
    InvalidUsername,
    AuthConfigUnavailable,
    CryptoFailure,
};

struct Bt1AuthError {
    Bt1AuthErrorKind kind = Bt1AuthErrorKind::InvalidToken;
    Bt1AuthFailureReason reason = Bt1AuthFailureReason::EmptyToken;
};

class Bt1Principal {
public:
    [[nodiscard]] std::string_view username() const noexcept { return {username_.data(), username_size_}; }
    [[nodiscard]] std::string_view kid() const noexcept { return {kid_.data(), kid_size_}; }
    [[nodiscard]] std::int64_t expires_at() const noexcept { return expires_at_; }

private:
    Bt1Principal(std::string_view username, std::string_view kid, std::int64_t expires_at) noexcept;

    friend std::expected<Bt1Principal, Bt1AuthError>
    verify_bt1_token(std::string_view token, const Bt1KeySnapshot &key_ring, std::int64_t now_seconds) noexcept;

    std::array<char, kBt1MaxUsernameBytes> username_{};
    std::array<char, kBt1MaxKidLength> kid_{};
    std::uint8_t username_size_ = 0;
    std::uint8_t kid_size_ = 0;
    std::int64_t expires_at_ = 0;
};

[[nodiscard]] std::expected<Bt1Principal, Bt1AuthError>
verify_bt1_token(std::string_view token, const Bt1KeySnapshot &key_ring, std::int64_t now_seconds) noexcept;

[[nodiscard]] std::string_view bt1_auth_error_name(Bt1AuthErrorKind kind) noexcept;
[[nodiscard]] std::string_view bt1_auth_error_message(Bt1AuthErrorKind kind) noexcept;

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_BT1_TOKEN_VERIFIER_H
