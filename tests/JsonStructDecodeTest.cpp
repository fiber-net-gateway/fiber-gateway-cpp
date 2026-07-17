#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string_view>

#include "common/json/JsonStructDecode.h"

namespace json_struct_test {

struct Base {
    Base() noexcept { request_id.set_null(); }

    fiber::json::Nullable<std::string_view> request_id;
};

struct Item {
    Item() noexcept { value.set_null(); }

    fiber::json::Nullable<std::int64_t> value;
};

struct Config : Base {
    Config() noexcept { name.set_null(); }

    fiber::json::Nullable<std::string_view> name;
    std::optional<std::int64_t> limit;
    bool enabled = false;
    fiber::json::JsonArray<Item> items;
    fiber::json::JsonObject<bool> flags;
};

struct RequiredConfig {
    std::int64_t id = 0;
};

struct StrictConfig {
    std::int64_t value = 0;
};

struct CustomConfig {
    std::int64_t value = 0;
};

fiber::json::ParseStatus parse_positive(fiber::json::JsonParser &parser, fiber::mem::BufPool &pool,
                                        std::int64_t &out) noexcept {
    std::int64_t value = 0;
    if (fiber::json::parse_integer(parser, pool, value) != fiber::json::ParseStatus::Done) {
        return fiber::json::ParseStatus::Error;
    }
    if (value <= 0) {
        (void) parser.fail("expected positive integer");
        return fiber::json::ParseStatus::Error;
    }
    out = value;
    return fiber::json::ParseStatus::Done;
}

} // namespace json_struct_test

FIBER_JSON_STRUCT(json_struct_test::Base, FIBER_JSON_NAMED_OPTIONAL_FIELD(request_id, "requestId"));

FIBER_JSON_STRUCT(json_struct_test::Item, FIBER_JSON_OPTIONAL_FIELD(value));

FIBER_JSON_STRUCT(json_struct_test::Config, FIBER_JSON_BASE(json_struct_test::Base), FIBER_JSON_OPTIONAL_FIELD(name),
                  FIBER_JSON_OPTIONAL_FIELD(limit), FIBER_JSON_OPTIONAL_FIELD(enabled),
                  FIBER_JSON_OPTIONAL_FIELD(items), FIBER_JSON_OPTIONAL_FIELD(flags),
                  FIBER_JSON_OPTIONAL_CONSTANT("module", std::string_view("config"), "unexpected module"),
                  FIBER_JSON_OPTIONAL_IGNORED(bool, "success"));

FIBER_JSON_STRUCT(json_struct_test::RequiredConfig, FIBER_JSON_FIELD(id));

FIBER_JSON_STRUCT(json_struct_test::CustomConfig,
                  FIBER_JSON_OPTIONAL_CUSTOM_FIELD(value, "value", json_struct_test::parse_positive));

template<>
struct fiber::json::StructMetadata<json_struct_test::StrictConfig> {
    using Self = json_struct_test::StrictConfig;

    static constexpr StructDecodeOptions options{
            .unknown_fields = UnknownFieldPolicy::Reject,
            .duplicate_fields = DuplicateFieldPolicy::Reject,
    };
    static constexpr auto fields = define_struct<Self>(optional_field<&Self::value>("value"));
    static_assert(unique_field_names(fields));
};

namespace {

using fiber::json::JsonParser;
using fiber::json::ParseStatus;
using fiber::mem::BufPool;

template<typename T>
ParseStatus parse_document(std::string_view input, BufPool &pool, T &out, JsonParser &parser) {
    if (!parser.feed(input.data(), input.size())) {
        return ParseStatus::Error;
    }
    parser.finish();
    auto value_parser = [](JsonParser &value_parser, BufPool &value_pool, T &value) noexcept {
        return fiber::json::parse_value(value_parser, value_pool, value);
    };
    return fiber::json::parse_document(parser, pool, out, value_parser);
}

static_assert(fiber::json::struct_field_count<json_struct_test::Config> == 8);

TEST(JsonStructDecodeTest, ParsesRegisteredFieldsRecursivelyAndPreservesWirePolicies) {
    constexpr std::string_view Input =
            R"({"name":"first","name":null,"limit":7,"enabled":true,"items":[{"value":1},{}],)"
            R"("flags":{"fast":true},"module":"config","success":false,"future":{"nested":[1]}})";

    BufPool pool;
    JsonParser parser;
    json_struct_test::Config value;
    ASSERT_EQ(parse_document(Input, pool, value, parser), ParseStatus::Done);

    EXPECT_TRUE(value.request_id.is_absent());
    EXPECT_TRUE(value.name.is_null());
    ASSERT_TRUE(value.limit.has_value());
    EXPECT_EQ(*value.limit, 7);
    EXPECT_TRUE(value.enabled);
    ASSERT_EQ(value.items.size(), 2u);
    ASSERT_TRUE(value.items[0].value.is_present());
    EXPECT_EQ(value.items[0].value.value(), 1);
    EXPECT_TRUE(value.items[1].value.is_absent());
    ASSERT_EQ(value.flags.size(), 1u);
    EXPECT_EQ(value.flags[0].key, "fast");
    EXPECT_TRUE(value.flags[0].value);
}

TEST(JsonStructDecodeTest, RejectsMissingRequiredFieldTransactionally) {
    BufPool pool;
    JsonParser parser;
    json_struct_test::RequiredConfig value{.id = 42};

    EXPECT_EQ(parse_document("{}", pool, value, parser), ParseStatus::Error);
    EXPECT_EQ(value.id, 42);
    EXPECT_STREQ(parser.error().message, "missing required JSON field");
}

TEST(JsonStructDecodeTest, RejectsConstantMismatchTransactionally) {
    BufPool pool;
    JsonParser parser;
    json_struct_test::Config value;
    value.name.set_present("unchanged");

    EXPECT_EQ(parse_document(R"({"name":"changed","module":"naming"})", pool, value, parser), ParseStatus::Error);
    ASSERT_TRUE(value.name.is_present());
    EXPECT_EQ(value.name.value(), "unchanged");
    EXPECT_STREQ(parser.error().message, "unexpected module");
}

TEST(JsonStructDecodeTest, AppliesStrictUnknownAndDuplicatePolicies) {
    BufPool pool;

    {
        JsonParser parser;
        json_struct_test::StrictConfig value;
        EXPECT_EQ(parse_document(R"({"unknown":1})", pool, value, parser), ParseStatus::Error);
        EXPECT_STREQ(parser.error().message, "unknown JSON field");
    }

    {
        JsonParser parser;
        json_struct_test::StrictConfig value;
        EXPECT_EQ(parse_document(R"({"value":1,"value":2})", pool, value, parser), ParseStatus::Error);
        EXPECT_STREQ(parser.error().message, "duplicate JSON field");
    }
}

TEST(JsonStructDecodeTest, SupportsFieldSpecificParsers) {
    BufPool pool;
    JsonParser parser;
    json_struct_test::CustomConfig value{.value = 9};

    EXPECT_EQ(parse_document(R"({"value":-1})", pool, value, parser), ParseStatus::Error);
    EXPECT_EQ(value.value, 9);
    EXPECT_STREQ(parser.error().message, "expected positive integer");
}

} // namespace
