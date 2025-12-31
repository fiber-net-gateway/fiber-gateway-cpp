#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "common/json/JsonEncode.h"

using fiber::json::CallbackSink;
using fiber::json::Generator;
using fiber::json::OutputSink;

namespace {
class StringSink final : public OutputSink {
public:
    [[nodiscard]] bool write(const char *data, size_t len) override {
        if (len == 0) {
            return true;
        }
        if (!data) {
            return false;
        }
        output.append(data, len);
        return true;
    }

    void reset() override {
        output.clear();
    }

    std::string output;
};
} // namespace

TEST(GeneratorTest, MapWithValues) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string("name", 4), Generator::Result::OK);
    EXPECT_EQ(gen.string("fiber", 5), Generator::Result::OK);
    EXPECT_EQ(gen.map_close(), Generator::Result::OK);

    EXPECT_EQ(sink.output, "{\"name\":\"fiber\"}");
}

TEST(GeneratorTest, ArrayValues) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::OK);
    EXPECT_EQ(gen.bool_value(true), Generator::Result::OK);
    EXPECT_EQ(gen.null_value(), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);

    EXPECT_EQ(sink.output, "[1,true,null]");
}

TEST(GeneratorTest, BeautifyIndent) {
    StringSink sink;
    Generator gen(sink);
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

    EXPECT_EQ(sink.output, "{\n  \"a\": 1,\n  \"b\": [\n    \"x\"\n  ]\n}");
}

TEST(GeneratorTest, KeysMustBeString) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::KeysMustBeString);
}

TEST(GeneratorTest, ValidateUtf8) {
    StringSink sink;
    Generator gen(sink);
    gen.set_option(Generator::Option::ValidateUtf8, true);
    const char bad[] = {static_cast<char>(0xC3), static_cast<char>(0x28)};
    EXPECT_EQ(gen.string(bad, sizeof(bad)), Generator::Result::InvalidString);
}

TEST(GeneratorTest, InvalidDouble) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.double_value(std::nan("")), Generator::Result::InvalidValue);
}

TEST(GeneratorTest, PrintCallbackOutput) {
    std::string output;
    auto callback = [](void *ctx, const char *data, size_t len) -> int {
        auto *out = static_cast<std::string *>(ctx);
        out->append(data, len);
        return 0;
    };

    CallbackSink sink(callback, &output);
    Generator gen(sink);
    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string("x", 1), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);

    EXPECT_EQ(output, "[\"x\"]");
}

TEST(GeneratorTest, BinaryBase64) {
    StringSink sink;
    Generator gen(sink);
    const std::uint8_t data[] = {'M', 'a', 'n'};
    EXPECT_EQ(gen.binary(data, sizeof(data)), Generator::Result::OK);
    EXPECT_EQ(sink.output, "\"TWFu\"");
}
