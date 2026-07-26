#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/hmac.h>

#include "auth/Bt1TokenVerifier.h"
#include "auth/LlmRequestAuthenticator.h"
#include "config/LlmConfigCodec.h"
#include "config/LlmConfigSnapshot.h"
#include "http/HttpHeaders.h"

namespace {

using fiber::ai_server::authenticate_llm_request;
using fiber::ai_server::AuthenticatedLlmRequest;
using fiber::ai_server::Bt1AuthError;
using fiber::ai_server::Bt1AuthErrorKind;
using fiber::ai_server::Bt1AuthFailureReason;
using fiber::ai_server::Bt1Key;
using fiber::ai_server::Bt1KeySnapshot;
using fiber::ai_server::extract_llm_request_token;
using fiber::ai_server::LlmConfigSnapshot;
using fiber::ai_server::parse_bt1_key_config;
using fiber::ai_server::verify_bt1_token;

constexpr std::string_view kRandomBytes = "abcdefghijklmnop";
constexpr std::string_view kGoldenToken = "BT1.dbg1.YWxpY2U.1893456000.tsbrTXqvXLP4--6w1NPXoQ."
                                          "kuk9EcniJnKXpp_9xw2jRYhRFJ7DUm11-iGr-nOm_QQ";
constexpr std::string_view kGoldenKeyConfig =
        R"({"version":1,"data":{"clockSkewSec":60,"keys":[{"kid":"dbg1","secret":"base64:CWatjvYsDJDV2iBEUT0vUa/qXxJiWusajLN0CNPcPyY="}]}})";

std::string base64url_encode(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t pos = 0;
    while (pos + 3 <= bytes.size()) {
        const auto a = static_cast<std::uint8_t>(bytes[pos]);
        const auto b = static_cast<std::uint8_t>(bytes[pos + 1]);
        const auto c = static_cast<std::uint8_t>(bytes[pos + 2]);
        output.push_back(alphabet[a >> 2]);
        output.push_back(alphabet[((a & 0x03U) << 4) | (b >> 4)]);
        output.push_back(alphabet[((b & 0x0FU) << 2) | (c >> 6)]);
        output.push_back(alphabet[c & 0x3FU]);
        pos += 3;
    }
    if (pos < bytes.size()) {
        const auto a = static_cast<std::uint8_t>(bytes[pos]);
        output.push_back(alphabet[a >> 2]);
        if (pos + 1 == bytes.size()) {
            output.push_back(alphabet[(a & 0x03U) << 4]);
        } else {
            const auto b = static_cast<std::uint8_t>(bytes[pos + 1]);
            output.push_back(alphabet[((a & 0x03U) << 4) | (b >> 4)]);
            output.push_back(alphabet[(b & 0x0FU) << 2]);
        }
    }
    return output;
}

std::string sign_bt1(std::string signing_input, std::string_view secret) {
    std::array<std::uint8_t, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_size = 0;
    if (!HMAC(EVP_sha256(), secret.data(), secret.size(), reinterpret_cast<const std::uint8_t *>(signing_input.data()),
              signing_input.size(), digest.data(), &digest_size)) {
        return {};
    }
    signing_input.push_back('.');
    signing_input.append(
            base64url_encode({reinterpret_cast<const char *>(digest.data()), static_cast<std::size_t>(digest_size)}));
    return signing_input;
}

std::string issue_token(std::string_view username, std::string_view kid, std::string_view secret,
                        std::string_view expiration) {
    std::string signing_input = "BT1.";
    signing_input.append(kid);
    signing_input.push_back('.');
    signing_input.append(base64url_encode(username));
    signing_input.push_back('.');
    signing_input.append(expiration);
    signing_input.push_back('.');
    signing_input.append(base64url_encode(kRandomBytes));
    return sign_bt1(std::move(signing_input), secret);
}

std::string sign_fields(std::string_view version, std::string_view kid, std::string_view user,
                        std::string_view expiration, std::string_view random, std::string_view secret) {
    std::string signing_input(version);
    signing_input.push_back('.');
    signing_input.append(kid);
    signing_input.push_back('.');
    signing_input.append(user);
    signing_input.push_back('.');
    signing_input.append(expiration);
    signing_input.push_back('.');
    signing_input.append(random);
    return sign_bt1(std::move(signing_input), secret);
}

Bt1KeySnapshot make_key_ring(std::initializer_list<std::pair<std::string_view, std::string_view>> keys,
                             std::int32_t clock_skew_seconds = 0) {
    Bt1KeySnapshot snapshot;
    snapshot.clock_skew_seconds = clock_skew_seconds;
    for (const auto &[kid, secret]: keys) {
        snapshot.keys.push_back(Bt1Key{.kid = std::string(kid), .secret = std::string(secret)});
    }
    std::sort(snapshot.keys.begin(), snapshot.keys.end(),
              [](const Bt1Key &left, const Bt1Key &right) { return left.kid < right.kid; });
    return snapshot;
}

std::shared_ptr<const LlmConfigSnapshot> make_config(Bt1KeySnapshot keys, std::uint64_t generation = 1) {
    auto config = std::make_shared<LlmConfigSnapshot>();
    config->generation = generation;
    config->bt1_keys = std::make_shared<const Bt1KeySnapshot>(std::move(keys));
    return config;
}

void expect_error(const std::expected<fiber::ai_server::Bt1Principal, Bt1AuthError> &result, Bt1AuthErrorKind kind,
                  Bt1AuthFailureReason reason) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, kind);
    EXPECT_EQ(result.error().reason, reason);
}

TEST(Bt1TokenVerifierTest, AcceptsDocumentGoldenVector) {
    auto key_ring = parse_bt1_key_config(kGoldenKeyConfig, "golden");
    ASSERT_TRUE(key_ring) << key_ring.error().message;

    auto result = verify_bt1_token(kGoldenToken, *key_ring, 1'800'000'000);

    ASSERT_TRUE(result);
    EXPECT_EQ(result->username(), "alice");
    EXPECT_EQ(result->kid(), "dbg1");
    EXPECT_EQ(result->expires_at(), 1'893'456'000);
}

TEST(Bt1TokenVerifierTest, AppliesClockSkewAtInclusiveBoundary) {
    const Bt1KeySnapshot key_ring = make_key_ring({{"a1", "test-secret"}}, 10);
    const std::string token = issue_token("alice", "a1", "test-secret", "100");

    auto boundary = verify_bt1_token(token, key_ring, 110);
    ASSERT_TRUE(boundary);
    EXPECT_EQ(boundary->username(), "alice");

    auto expired = verify_bt1_token(token, key_ring, 111);
    expect_error(expired, Bt1AuthErrorKind::ExpiredToken, Bt1AuthFailureReason::Expired);
}

TEST(Bt1TokenVerifierTest, RejectsTamperedMacAndUnknownKid) {
    const Bt1KeySnapshot key_ring = make_key_ring({{"a1", "test-secret"}});
    std::string tampered = issue_token("alice", "a1", "test-secret", "1900000000");
    ASSERT_FALSE(tampered.empty());
    tampered.back() = tampered.back() == 'A' ? 'B' : 'A';

    auto bad_mac = verify_bt1_token(tampered, key_ring, 1'800'000'000);
    expect_error(bad_mac, Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::MacMismatch);

    const std::string unknown = issue_token("alice", "b2", "test-secret", "1900000000");
    auto unknown_kid = verify_bt1_token(unknown, key_ring, 1'800'000'000);
    expect_error(unknown_kid, Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::UnknownKid);
}

TEST(Bt1TokenVerifierTest, RejectsMalformedStructureAndFields) {
    const Bt1KeySnapshot key_ring = make_key_ring({{"a1", "test-secret"}});
    const std::string random = base64url_encode(kRandomBytes);

    expect_error(verify_bt1_token({}, key_ring, 0), Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::EmptyToken);
    expect_error(verify_bt1_token(std::string(513, 'x'), key_ring, 0), Bt1AuthErrorKind::InvalidToken,
                 Bt1AuthFailureReason::TokenTooLong);
    expect_error(verify_bt1_token("BT1.bad", key_ring, 0), Bt1AuthErrorKind::InvalidToken,
                 Bt1AuthFailureReason::InvalidSegmentCount);

    expect_error(verify_bt1_token(sign_fields("BT2", "a1", "YQ", "100", random, "test-secret"), key_ring, 0),
                 Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidVersion);
    expect_error(verify_bt1_token(sign_fields("BT1", "bad!", "YQ", "100", random, "test-secret"), key_ring, 0),
                 Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidKid);
    expect_error(verify_bt1_token(sign_fields("BT1", "a1", "YQ=", "100", random, "test-secret"), key_ring, 0),
                 Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidUserEncoding);
    expect_error(verify_bt1_token(sign_fields("BT1", "a1", "YQ", "100", "short", "test-secret"), key_ring, 0),
                 Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidRandomEncoding);

    std::string invalid_mac = issue_token("a", "a1", "test-secret", "100");
    ASSERT_FALSE(invalid_mac.empty());
    invalid_mac.back() = '=';
    expect_error(verify_bt1_token(invalid_mac, key_ring, 0), Bt1AuthErrorKind::InvalidToken,
                 Bt1AuthFailureReason::InvalidMacEncoding);
    expect_error(verify_bt1_token(sign_fields("BT1", "a1", "YQ", "0100", random, "test-secret"), key_ring, 0),
                 Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidExpiration);
    expect_error(
            verify_bt1_token(sign_fields("BT1", "a1", "YQ", "9223372036854775808", random, "test-secret"), key_ring, 0),
            Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidExpiration);
}

TEST(Bt1TokenVerifierTest, ValidatesUsernameAfterSignature) {
    const Bt1KeySnapshot key_ring = make_key_ring({{"a1", "test-secret"}});
    const std::string username64(64, 'u');
    const std::string username65(65, 'u');
    const std::string malformed_utf8("\xC3\x28", 2);

    auto max_username =
            verify_bt1_token(issue_token(username64, "a1", "test-secret", "1900000000"), key_ring, 1'800'000'000);
    ASSERT_TRUE(max_username);
    EXPECT_EQ(max_username->username(), username64);

    auto too_long =
            verify_bt1_token(issue_token(username65, "a1", "test-secret", "1900000000"), key_ring, 1'800'000'000);
    expect_error(too_long, Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidUsername);

    auto invalid_utf8 =
            verify_bt1_token(issue_token(malformed_utf8, "a1", "test-secret", "1900000000"), key_ring, 1'800'000'000);
    expect_error(invalid_utf8, Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidUsername);

    auto invalid_base64_length =
            verify_bt1_token(sign_fields("BT1", "a1", "A", "1900000000", base64url_encode(kRandomBytes), "test-secret"),
                             key_ring, 1'800'000'000);
    expect_error(invalid_base64_length, Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidUsername);

    auto empty = verify_bt1_token(issue_token({}, "a1", "test-secret", "1900000000"), key_ring, 1'800'000'000);
    expect_error(empty, Bt1AuthErrorKind::InvalidToken, Bt1AuthFailureReason::InvalidSegmentCount);
}

TEST(Bt1TokenVerifierTest, HandlesMaximumExpirationWithoutOverflow) {
    const Bt1KeySnapshot key_ring = make_key_ring({{"a1", "test-secret"}}, 300);
    const std::string token = issue_token("alice", "a1", "test-secret", "9223372036854775807");

    auto result = verify_bt1_token(token, key_ring, std::numeric_limits<std::int64_t>::max());

    ASSERT_TRUE(result);
    EXPECT_EQ(result->expires_at(), std::numeric_limits<std::int64_t>::max());
}

TEST(Bt1TokenVerifierTest, SupportsKeyRotation) {
    const std::string old_token = issue_token("alice", "old", "old-secret", "1900000000");
    const std::string new_token = issue_token("bob", "new", "new-secret", "1900000000");
    const Bt1KeySnapshot rotating = make_key_ring({{"old", "old-secret"}, {"new", "new-secret"}});

    ASSERT_TRUE(verify_bt1_token(old_token, rotating, 1'800'000'000));
    ASSERT_TRUE(verify_bt1_token(new_token, rotating, 1'800'000'000));

    const Bt1KeySnapshot rotated = make_key_ring({{"new", "new-secret"}});
    expect_error(verify_bt1_token(old_token, rotated, 1'800'000'000), Bt1AuthErrorKind::InvalidToken,
                 Bt1AuthFailureReason::UnknownKid);
    ASSERT_TRUE(verify_bt1_token(new_token, rotated, 1'800'000'000));
}

TEST(LlmRequestAuthenticatorTest, AcceptsCaseInsensitiveBearerAndPinsSnapshot) {
    auto parsed = parse_bt1_key_config(kGoldenKeyConfig, "golden");
    ASSERT_TRUE(parsed);
    const auto config = make_config(std::move(*parsed), 7);
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    ASSERT_NE(headers.set("Authorization", std::string("bEaReR \t") + std::string(kGoldenToken) + " \t"), nullptr);

    auto result = authenticate_llm_request(headers, config, 1'800'000'000);

    ASSERT_TRUE(result);
    EXPECT_EQ(result->config_handle(), config);
    EXPECT_EQ(result->config().generation, 7);
    EXPECT_EQ(result->principal().username(), "alice");
    EXPECT_EQ(result->principal().kid(), "dbg1");
}

TEST(LlmRequestAuthenticatorTest, FallsBackToApiKeyButAuthorizationTakesPrecedence) {
    auto parsed = parse_bt1_key_config(kGoldenKeyConfig, "golden");
    ASSERT_TRUE(parsed);
    const auto config = make_config(std::move(*parsed));

    fiber::mem::BufPool fallback_pool;
    fiber::http::HttpHeaders fallback_headers(fallback_pool);
    ASSERT_NE(fallback_headers.set("x-api-key", kGoldenToken), nullptr);
    auto fallback = authenticate_llm_request(fallback_headers, config, 1'800'000'000);
    ASSERT_TRUE(fallback);
    EXPECT_EQ(fallback->principal().username(), "alice");

    fiber::mem::BufPool precedence_pool;
    fiber::http::HttpHeaders precedence_headers(precedence_pool);
    ASSERT_NE(precedence_headers.set("Authorization", "Basic ignored"), nullptr);
    ASSERT_NE(precedence_headers.set("x-api-key", kGoldenToken), nullptr);
    auto precedence = authenticate_llm_request(precedence_headers, config, 1'800'000'000);
    ASSERT_FALSE(precedence);
    EXPECT_EQ(precedence.error().kind, Bt1AuthErrorKind::InvalidToken);
    EXPECT_EQ(precedence.error().reason, Bt1AuthFailureReason::InvalidAuthorizationScheme);

    fiber::mem::BufPool empty_authorization_pool;
    fiber::http::HttpHeaders empty_authorization_headers(empty_authorization_pool);
    ASSERT_NE(empty_authorization_headers.set("Authorization", ""), nullptr);
    ASSERT_NE(empty_authorization_headers.set("x-api-key", kGoldenToken), nullptr);
    EXPECT_TRUE(authenticate_llm_request(empty_authorization_headers, config, 1'800'000'000));
}

TEST(LlmRequestAuthenticatorTest, RejectsMissingEmptyAndBareAuthorization) {
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);

    auto missing = extract_llm_request_token(headers);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().reason, Bt1AuthFailureReason::MissingCredential);

    ASSERT_NE(headers.set("Authorization", "Bearer   "), nullptr);
    auto empty = extract_llm_request_token(headers);
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().reason, Bt1AuthFailureReason::MissingCredential);

    ASSERT_NE(headers.set("Authorization", kGoldenToken), nullptr);
    auto bare = extract_llm_request_token(headers);
    ASSERT_FALSE(bare);
    EXPECT_EQ(bare.error().reason, Bt1AuthFailureReason::InvalidAuthorizationScheme);
}

TEST(LlmRequestAuthenticatorTest, RejectsUnavailableConfigAsInternalFailure) {
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    ASSERT_NE(headers.set("Authorization", std::string("Bearer ") + std::string(kGoldenToken)), nullptr);

    auto missing_config = authenticate_llm_request(headers, nullptr, 1'800'000'000);
    ASSERT_FALSE(missing_config);
    EXPECT_EQ(missing_config.error().kind, Bt1AuthErrorKind::InternalError);
    EXPECT_EQ(missing_config.error().reason, Bt1AuthFailureReason::AuthConfigUnavailable);

    auto empty_config = std::make_shared<LlmConfigSnapshot>();
    auto missing_keys = authenticate_llm_request(headers, std::move(empty_config), 1'800'000'000);
    ASSERT_FALSE(missing_keys);
    EXPECT_EQ(missing_keys.error().kind, Bt1AuthErrorKind::InternalError);
    EXPECT_EQ(missing_keys.error().reason, Bt1AuthFailureReason::AuthConfigUnavailable);
}

TEST(LlmRequestAuthenticatorTest, KeepsInflightSnapshotStableAcrossRotation) {
    const std::string old_token = issue_token("alice", "old", "old-secret", "1900000000");
    const std::string new_token = issue_token("bob", "new", "new-secret", "1900000000");
    const auto old_config = make_config(make_key_ring({{"old", "old-secret"}}), 1);
    const auto new_config = make_config(make_key_ring({{"new", "new-secret"}}), 2);

    fiber::mem::BufPool old_pool;
    fiber::http::HttpHeaders old_headers(old_pool);
    ASSERT_NE(old_headers.set("Authorization", std::string("Bearer ") + old_token), nullptr);
    auto inflight = authenticate_llm_request(old_headers, old_config, 1'800'000'000);
    ASSERT_TRUE(inflight);

    auto rejected_old = authenticate_llm_request(old_headers, new_config, 1'800'000'000);
    ASSERT_FALSE(rejected_old);
    EXPECT_EQ(rejected_old.error().reason, Bt1AuthFailureReason::UnknownKid);

    fiber::mem::BufPool new_pool;
    fiber::http::HttpHeaders new_headers(new_pool);
    ASSERT_NE(new_headers.set("Authorization", std::string("Bearer ") + new_token), nullptr);
    auto accepted_new = authenticate_llm_request(new_headers, new_config, 1'800'000'000);
    ASSERT_TRUE(accepted_new);
    EXPECT_EQ(accepted_new->principal().username(), "bob");

    EXPECT_EQ(inflight->config_handle(), old_config);
    EXPECT_EQ(inflight->config().generation, 1);
    EXPECT_EQ(inflight->principal().username(), "alice");
    EXPECT_NE(inflight->config_handle(), accepted_new->config_handle());
}

TEST(LlmRequestAuthenticatorTest, ExposesStablePublicErrorNamesAndMessages) {
    EXPECT_EQ(fiber::ai_server::bt1_auth_error_name(Bt1AuthErrorKind::InvalidToken), "invalid_token");
    EXPECT_EQ(fiber::ai_server::bt1_auth_error_message(Bt1AuthErrorKind::InvalidToken), "invalid bearer token");
    EXPECT_EQ(fiber::ai_server::bt1_auth_error_name(Bt1AuthErrorKind::ExpiredToken), "expired_token");
    EXPECT_EQ(fiber::ai_server::bt1_auth_error_message(Bt1AuthErrorKind::ExpiredToken), "expired bearer token");
}

} // namespace
