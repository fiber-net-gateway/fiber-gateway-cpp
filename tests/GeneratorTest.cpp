#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "common/json/JsonEncode.h"

using fiber::json::Generator;

TEST(GeneratorTest, MapWithValues) {
    Generator gen;
    EXPECT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string("name", 4), Generator::Result::OK);
    EXPECT_EQ(gen.string("fiber", 5), Generator::Result::OK);
    EXPECT_EQ(gen.map_close(), Generator::Result::OK);

    const unsigned char *buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gen.get_buf(&buf, &len), Generator::Result::OK);
    ASSERT_NE(buf, nullptr);
    std::string output(reinterpret_cast<const char *>(buf), len);
    EXPECT_EQ(output, "{\"name\":\"fiber\"}");
}

TEST(GeneratorTest, ArrayValues) {
    Generator gen;
    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::OK);
    EXPECT_EQ(gen.bool_value(true), Generator::Result::OK);
    EXPECT_EQ(gen.null_value(), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);

    const unsigned char *buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gen.get_buf(&buf, &len), Generator::Result::OK);
    std::string output(reinterpret_cast<const char *>(buf), len);
    EXPECT_EQ(output, "[1,true,null]");
}

TEST(GeneratorTest, BeautifyIndent) {
    Generator gen;
    gen.set_option(Generator::Option::Beauty, true);
    gen.set_indent_string("  ");

    EXPECT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string("a", 1), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::OK);
    EXPECT_EQ(gen.string("b", 1), Generator::Result::OK);
    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string("x", 1), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);
    EXPECT_EQ(gen.map_close(), Generator::Result::OK);

    const unsigned char *buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gen.get_buf(&buf, &len), Generator::Result::OK);
    std::string output(reinterpret_cast<const char *>(buf), len);
    EXPECT_EQ(output, "{\n  \"a\": 1,\n  \"b\": [\n    \"x\"\n  ]\n}");
}

TEST(GeneratorTest, KeysMustBeString) {
    Generator gen;
    EXPECT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::KeysMustBeString);
}

TEST(GeneratorTest, ValidateUtf8) {
    Generator gen;
    gen.set_option(Generator::Option::ValidateUtf8, true);
    const char bad[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    EXPECT_EQ(gen.string(bad, sizeof(bad)), Generator::Result::InvalidString);
}

TEST(GeneratorTest, InvalidDouble) {
    Generator gen;
    EXPECT_EQ(gen.double_value(std::nan("")), Generator::Result::InvalidValue);
}

TEST(GeneratorTest, PrintCallbackOutput) {
    Generator gen;
    std::string output;
    auto callback = [](void *ctx, const char *data, size_t len) -> int {
        auto *out = static_cast<std::string *>(ctx);
        out->append(data, len);
        return 0;
    };

    gen.set_print_callback(callback, &output);
    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string("x", 1), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);

    const unsigned char *buf = nullptr;
    size_t len = 0;
    EXPECT_EQ(gen.get_buf(&buf, &len), Generator::Result::NoBuf);
    EXPECT_EQ(output, "[\"x\"]");
}
