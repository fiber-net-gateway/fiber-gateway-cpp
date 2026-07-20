#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include <fiber/nacos/dto/JsonCodec.h>

namespace {

using fiber::json::Generator;
using fiber::json::JsonParser;
using fiber::json::OutputSink;
using fiber::json::ParseStatus;
using fiber::mem::BufPool;
using fiber::nacos::dto::req::ConfigQueryRequest;
using fiber::nacos::dto::resp::AuthTokenResponse;
using fiber::nacos::dto::resp::NotifySubscriberResponse;

class StringSink final : public OutputSink {
public:
    [[nodiscard]] bool write(const char *data, std::size_t len) override {
        if (!data && len > 0) {
            return false;
        }
        output.append(data ? data : "", len);
        return true;
    }

    void reset() override { output.clear(); }

    std::string output;
};

ParseStatus parse_config_query_request(JsonParser &parser, BufPool &pool, ConfigQueryRequest &out) noexcept {
    return fiber::nacos::dto::parse_json(parser, pool, out);
}

ParseStatus parse_notify_subscriber_response(JsonParser &parser, BufPool &pool,
                                             NotifySubscriberResponse &out) noexcept {
    return fiber::nacos::dto::parse_json(parser, pool, out);
}

ParseStatus parse_auth_token_response(JsonParser &parser, BufPool &pool, AuthTokenResponse &out) noexcept {
    return fiber::nacos::dto::parse_json(parser, pool, out);
}

template<auto VP, typename T>
ParseStatus parse_complete(std::string_view input, BufPool &pool, T &out, JsonParser &parser) {
    if (!parser.feed(input.data(), input.size())) {
        return ParseStatus::Error;
    }
    parser.finish();
    return fiber::json::parse_document<VP>(parser, pool, out);
}

template<typename T>
std::string encode(const T &value) {
    StringSink sink;
    Generator generator(sink);
    EXPECT_EQ(fiber::nacos::dto::encode_json(generator, value), Generator::Result::OK);
    return sink.output;
}

TEST(NacosDtoJsonTest, ConfigQueryRequestMatchesJavaWireJson) {
    constexpr std::string_view JavaJson =
            R"({"requestId":null,"dataId":"data","group":"group","tenant":"tenant","tag":null,"notify":false,"module":"config"})";

    ConfigQueryRequest built = ConfigQueryRequest::build("data", "group", "tenant");
    EXPECT_EQ(ConfigQueryRequest::kTypeName, "ConfigQueryRequest");
    EXPECT_EQ(encode(built), JavaJson);

    BufPool pool;
    JsonParser parser;
    std::string input(JavaJson);
    ConfigQueryRequest decoded;
    ASSERT_EQ(parse_complete<parse_config_query_request>(input, pool, decoded, parser), ParseStatus::Done);
    std::fill(input.begin(), input.end(), '!');

    EXPECT_TRUE(decoded.request_id.is_null());
    ASSERT_TRUE(decoded.data_id.is_present());
    EXPECT_EQ(decoded.data_id.value(), "data");
    ASSERT_TRUE(decoded.group.is_present());
    EXPECT_EQ(decoded.group.value(), "group");
    ASSERT_TRUE(decoded.tenant.is_present());
    EXPECT_EQ(decoded.tenant.value(), "tenant");
    EXPECT_TRUE(decoded.tag.is_null());
    EXPECT_FALSE(decoded.notify);
    EXPECT_EQ(encode(decoded), JavaJson);
}

TEST(NacosDtoJsonTest, ConfigQueryRequestDefaultMatchesJavaDefaultObject) {
    ConfigQueryRequest value;
    EXPECT_EQ(
            encode(value),
            R"({"requestId":null,"dataId":null,"group":null,"tenant":null,"tag":null,"notify":false,"module":"config"})");
}

TEST(NacosDtoJsonTest, ConfigQueryRequestPreservesAbsentAndSkipsUnknownFields) {
    constexpr std::string_view Input = R"({"notify":true,"module":"config","future":{"nested":[1,true,null]}})";

    BufPool pool;
    JsonParser parser;
    ConfigQueryRequest value;
    ASSERT_EQ(parse_complete<parse_config_query_request>(Input, pool, value, parser), ParseStatus::Done);

    EXPECT_TRUE(value.request_id.is_absent());
    EXPECT_TRUE(value.data_id.is_absent());
    EXPECT_TRUE(value.group.is_absent());
    EXPECT_TRUE(value.tenant.is_absent());
    EXPECT_TRUE(value.tag.is_absent());
    EXPECT_TRUE(value.notify);
    EXPECT_EQ(encode(value), R"({"notify":true,"module":"config"})");
}

TEST(NacosDtoJsonTest, ConfigQueryRequestRejectsWrongTypeAndModuleTransactionally) {
    BufPool pool;

    {
        ConfigQueryRequest value = ConfigQueryRequest::build("old-data", "old-group", "old-tenant");
        JsonParser parser;
        EXPECT_EQ(parse_complete<parse_config_query_request>(R"({"dataId":{}})", pool, value, parser),
                  ParseStatus::Error);
        ASSERT_TRUE(value.data_id.is_present());
        EXPECT_EQ(value.data_id.value(), "old-data");
        EXPECT_STREQ(parser.error().message, "expected string");
    }

    {
        ConfigQueryRequest value = ConfigQueryRequest::build("old-data", "old-group", "old-tenant");
        JsonParser parser;
        EXPECT_EQ(parse_complete<parse_config_query_request>(R"({"module":"naming"})", pool, value, parser),
                  ParseStatus::Error);
        ASSERT_TRUE(value.data_id.is_present());
        EXPECT_EQ(value.data_id.value(), "old-data");
        EXPECT_STREQ(parser.error().message, "unexpected Nacos request module");
    }
}

TEST(NacosDtoJsonTest, NotifySubscriberResponseMatchesJavaWireJson) {
    constexpr std::string_view JavaJson =
            R"({"resultCode":200,"errorCode":0,"message":null,"requestId":null,"success":true})";

    EXPECT_EQ(NotifySubscriberResponse::kTypeName, "NotifySubscriberResponse");
    EXPECT_EQ(encode(NotifySubscriberResponse{}), JavaJson);

    BufPool pool;
    JsonParser parser;
    NotifySubscriberResponse value;
    ASSERT_EQ(parse_complete<parse_notify_subscriber_response>(JavaJson, pool, value, parser), ParseStatus::Done);

    EXPECT_EQ(value.result_code, 200);
    EXPECT_EQ(value.error_code, 0);
    EXPECT_TRUE(value.message.is_null());
    EXPECT_TRUE(value.request_id.is_null());
    EXPECT_TRUE(value.success());
    EXPECT_EQ(encode(value), JavaJson);
}

TEST(NacosDtoJsonTest, NotifySubscriberResponseUsesDefaultsAndComputedSuccess) {
    BufPool pool;
    JsonParser parser;
    NotifySubscriberResponse value;
    ASSERT_EQ(parse_complete<parse_notify_subscriber_response>(
                      R"({"resultCode":500,"message":"failed","success":true,"future":1})", pool, value, parser),
              ParseStatus::Done);

    EXPECT_EQ(value.result_code, 500);
    EXPECT_EQ(value.error_code, 0);
    ASSERT_TRUE(value.message.is_present());
    EXPECT_EQ(value.message.value(), "failed");
    EXPECT_TRUE(value.request_id.is_absent());
    EXPECT_FALSE(value.success());
    EXPECT_EQ(encode(value), R"({"resultCode":500,"errorCode":0,"message":"failed","success":false})");

    JsonParser empty_parser;
    NotifySubscriberResponse empty;
    ASSERT_EQ(parse_complete<parse_notify_subscriber_response>("{}", pool, empty, empty_parser), ParseStatus::Done);
    EXPECT_TRUE(empty.message.is_absent());
    EXPECT_TRUE(empty.request_id.is_absent());
    EXPECT_EQ(encode(empty), R"({"resultCode":200,"errorCode":0,"success":true})");
}

TEST(NacosDtoJsonTest, NotifySubscriberResponseRejectsInvalidFieldTypesTransactionally) {
    BufPool pool;
    JsonParser parser;
    NotifySubscriberResponse value;
    value.result_code = 500;
    value.error_code = 123;
    value.message.set_present("unchanged");

    EXPECT_EQ(parse_complete<parse_notify_subscriber_response>(R"({"resultCode":2147483648})", pool, value, parser),
              ParseStatus::Error);
    EXPECT_EQ(value.result_code, 500);
    EXPECT_EQ(value.error_code, 123);
    ASSERT_TRUE(value.message.is_present());
    EXPECT_EQ(value.message.value(), "unchanged");
    EXPECT_STREQ(parser.error().message, "integer out of range");
}

TEST(NacosDtoJsonTest, AuthTokenResponseParsesLoginPayload) {
    constexpr std::string_view Input =
            R"({"accessToken":"token-value","tokenTtl":18000,"globalAdmin":true,"username":"nacos","future":1})";

    BufPool pool;
    JsonParser parser;
    std::string input(Input);
    AuthTokenResponse value;
    ASSERT_EQ(parse_complete<parse_auth_token_response>(input, pool, value, parser), ParseStatus::Done);
    std::fill(input.begin(), input.end(), '!');

    ASSERT_TRUE(value.access_token.is_present());
    EXPECT_EQ(value.access_token.value(), "token-value");
    EXPECT_EQ(value.token_ttl, 18000);
}

TEST(NacosDtoJsonTest, AuthTokenResponseTracksMissingAccessToken) {
    BufPool pool;
    JsonParser parser;
    AuthTokenResponse value;
    ASSERT_EQ(parse_complete<parse_auth_token_response>(R"({"tokenTtl":10})", pool, value, parser), ParseStatus::Done);
    EXPECT_TRUE(value.access_token.is_absent());
    EXPECT_EQ(value.token_ttl, 10);
}

TEST(NacosDtoJsonTest, AuthTokenResponseRejectsWrongTypesTransactionally) {
    BufPool pool;
    JsonParser parser;
    AuthTokenResponse value;
    value.access_token.set_present("old-token");
    value.token_ttl = 15;

    EXPECT_EQ(parse_complete<parse_auth_token_response>(R"({"tokenTtl":"bad"})", pool, value, parser),
              ParseStatus::Error);
    ASSERT_TRUE(value.access_token.is_present());
    EXPECT_EQ(value.access_token.value(), "old-token");
    EXPECT_EQ(value.token_ttl, 15);
    EXPECT_STREQ(parser.error().message, "expected integer");
}

} // namespace
