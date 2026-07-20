#include <gtest/gtest.h>

#include <string_view>

#include <fiber/nacos/dto/ConfigQueryRequest.h>
#include <fiber/nacos/dto/Internal.h>

#include "../src/rpc/NacosPayloadCodec.h"

namespace {

namespace dto = fiber::nacos::dto;
namespace detail = fiber::nacos::detail;
namespace proto = fiber::nacos::proto;

detail::NacosPayloadMetadata metadata() {
    return {
            .client_ip = "127.0.0.1",
            .client_version = "fiber-test",
            .namespace_id = "namespace",
            .access_token = "token",
    };
}

TEST(NacosPayloadTest, EncodesJavaCompatibleEnvelopeAndMetadata) {
    const auto request = dto::req::ConfigQueryRequest::build("data", "group", "tenant");
    auto payload = detail::encode_payload(request, metadata(), 4096);
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload->has_metadata());
    EXPECT_EQ(payload->metadata().type(), "ConfigQueryRequest");
    EXPECT_EQ(payload->metadata().clientip(), "127.0.0.1");
    EXPECT_EQ(payload->metadata().headers().at("clientIp"), "127.0.0.1");
    EXPECT_EQ(payload->metadata().headers().at("clientVersion"), "fiber-test");
    EXPECT_EQ(payload->metadata().headers().at("namespace"), "namespace");
    EXPECT_EQ(payload->metadata().headers().at("accessToken"), "token");
    EXPECT_TRUE(payload->body().type_url().empty());
    EXPECT_EQ(
            payload->body().value(),
            R"({"requestId":null,"dataId":"data","group":"group","tenant":"tenant","tag":null,"notify":false,"module":"config"})");
}

TEST(NacosPayloadTest, OmitsAccessTokenWhenAuthenticationIsNotConfigured) {
    auto no_auth_metadata = metadata();
    no_auth_metadata.access_token.reset();
    const auto request = dto::req::ConfigQueryRequest::build("data", "group", "tenant");
    auto payload = detail::encode_payload(request, no_auth_metadata, 4096);
    ASSERT_TRUE(payload.has_value());
    EXPECT_FALSE(payload->metadata().headers().contains("accessToken"));
}

TEST(NacosPayloadTest, ParsesFixedWireFixtureWithNacosFieldNumbers) {
    constexpr std::string_view Wire("\x12\x14\x1a\x12ServerCheckRequest\x1a\x09\x12\x07{\"x\":1}", 33);
    proto::Payload payload;
    ASSERT_TRUE(payload.ParseFromArray(Wire.data(), static_cast<int>(Wire.size())));
    auto view = detail::validate_payload(payload, 4096);
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->type, "ServerCheckRequest");
    EXPECT_EQ(view->json, R"({"x":1})");
    EXPECT_TRUE(payload.body().type_url().empty());
}

TEST(NacosPayloadTest, DecodesResponseAndMapsErrorResponse) {
    dto::resp::ServerCheckResponse wire_response;
    wire_response.connection_id.set_present("connection");
    wire_response.support_ability_negotiation = true;
    auto payload = detail::encode_payload(wire_response, metadata(), 4096);
    ASSERT_TRUE(payload.has_value());

    fiber::mem::BufPool pool;
    dto::resp::ServerCheckResponse decoded;
    auto result = detail::decode_payload(*payload, 4096, pool, decoded);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(decoded.connection_id.is_present());
    EXPECT_EQ(decoded.connection_id.value(), "connection");
    EXPECT_TRUE(decoded.support_ability_negotiation);

    dto::resp::ErrorResponse wire_error;
    wire_error.result_code = 500;
    wire_error.error_code = 300;
    wire_error.message.set_present("missing");
    auto error_payload = detail::encode_payload(wire_error, metadata(), 4096);
    ASSERT_TRUE(error_payload.has_value());
    fiber::mem::BufPool error_pool;
    dto::resp::ServerCheckResponse ignored;
    auto error = detail::decode_payload(*error_payload, 4096, error_pool, ignored);
    ASSERT_FALSE(error.has_value());
    EXPECT_EQ(error.error().code, detail::NacosRpcErrorCode::Server);
    EXPECT_EQ(error.error().result_code, 500);
    EXPECT_EQ(error.error().error_code, 300);
    EXPECT_EQ(error.error().message, "missing");
}

TEST(NacosPayloadTest, RejectsTypeMismatchTypeUrlAndLimits) {
    auto payload = detail::encode_payload(dto::resp::HealthCheckResponse{}, metadata(), 4096);
    ASSERT_TRUE(payload.has_value());
    fiber::mem::BufPool pool;
    dto::resp::ServerCheckResponse response;
    auto mismatch = detail::decode_payload(*payload, 4096, pool, response);
    ASSERT_FALSE(mismatch.has_value());
    EXPECT_EQ(mismatch.error().code, detail::NacosRpcErrorCode::Protocol);

    payload->mutable_body()->set_type_url("type.googleapis.com/unsupported");
    auto invalid_any = detail::validate_payload(*payload, 4096);
    ASSERT_FALSE(invalid_any.has_value());
    EXPECT_EQ(invalid_any.error().io_error, fiber::common::IoErr::Invalid);

    auto too_large = detail::encode_payload(dto::req::ServerCheckRequest{}, metadata(), 8);
    ASSERT_FALSE(too_large.has_value());
    EXPECT_EQ(too_large.error().io_error, fiber::common::IoErr::MessageTooLarge);
}

} // namespace
