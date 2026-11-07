#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/common/util/Base64.h>

#include "support/AllocFailure.h"

using fiber::json::CallbackSink;
using fiber::json::Generator;
using fiber::json::OutputSink;

namespace {
class StringSink final : public OutputSink {
public:
    [[nodiscard]] bool write(const char *data, size_t len) noexcept override {
        if (len == 0) {
            return true;
        }
        if (!data) {
            return false;
        }
        output.append(data, len);
        return true;
    }

    void reset() noexcept override { output.clear(); }

    std::string output;
};

// Keeps nothing, so a test can tell allocations made by the generator from the sink's.
class CountingSink final : public OutputSink {
public:
    [[nodiscard]] bool write(const char * /*data*/, size_t len) noexcept override {
        size += len;
        return true;
    }

    size_t size = 0;
};

std::vector<std::uint8_t> patterned_bytes(size_t len) {
    std::vector<std::uint8_t> data(len);
    for (size_t i = 0; i < len; ++i) {
        data[i] = static_cast<std::uint8_t>((i * 37) + 11);
    }
    return data;
}
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
    ASSERT_TRUE(gen.set_indent_string("  "));

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

TEST(GeneratorTest, StringFromChunksEscapesAcrossChunks) {
    struct ChunkCtx {
        const char *chunks[2];
        size_t lens[2];
        size_t index = 0;
    };
    ChunkCtx ctx{.chunks = {"a", "\"b"}, .lens = {1, 2}};
    auto producer = [](void *raw, const char *&data, size_t &len, bool &done) noexcept -> bool {
        auto *self = static_cast<ChunkCtx *>(raw);
        if (!self || self->index > 2) {
            return false;
        }
        if (self->index == 2) {
            data = nullptr;
            len = 0;
            done = true;
            return true;
        }
        data = self->chunks[self->index];
        len = self->lens[self->index];
        done = false;
        self->index += 1;
        return true;
    };

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.string_from_chunks(producer, &ctx), Generator::Result::OK);
    EXPECT_EQ(sink.output, "\"a\\\"b\"");
}

TEST(GeneratorTest, InvalidDouble) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.double_value(std::nan("")), Generator::Result::InvalidValue);
}

TEST(GeneratorTest, PrintCallbackOutput) {
    std::string output;
    auto callback = [](void *ctx, const char *data, size_t len) noexcept -> int {
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

TEST(GeneratorTest, BinaryMatchesBase64AcrossChunkBoundaries) {
    // Generator::binary encodes 768-byte chunks; cover both sides of the boundaries and every
    // padding length.
    for (const size_t len: {0U, 1U, 2U, 3U, 4U, 767U, 768U, 769U, 770U, 1535U, 1536U, 1537U, 5000U}) {
        const std::vector<std::uint8_t> data = patterned_bytes(len);
        StringSink sink;
        Generator gen(sink);
        EXPECT_EQ(gen.binary(data.data(), data.size()), Generator::Result::OK) << len;
        EXPECT_EQ(sink.output, "\"" + fiber::util::base64_encode(data.data(), data.size()) + "\"") << len;
    }
}

TEST(GeneratorTest, BinaryAcceptsNullEmptyData) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.binary(nullptr, 0), Generator::Result::OK);
    EXPECT_EQ(sink.output, "\"\"");
}

TEST(GeneratorTest, BinaryHonorsEscapeSolidus) {
    const std::uint8_t data[] = {0xFF, 0xFF, 0xFF}; // encodes to "////"
    StringSink sink;
    Generator gen(sink);
    gen.set_option(Generator::Option::EscapeSolidus, true);
    EXPECT_EQ(gen.binary(data, sizeof(data)), Generator::Result::OK);
    EXPECT_EQ(sink.output, R"("\/\/\/\/")");
}

TEST(GeneratorTest, BinaryDoesNotAllocate) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    const std::vector<std::uint8_t> data = patterned_bytes(10000);
    CountingSink sink;
    Generator gen(sink);
    Generator::Result result = Generator::Result::ErrorState;
    {
        // An allocation here would throw through noexcept and terminate the test.
        fiber::test::ScopedAllocFailure no_allocations(0);
        result = gen.binary(data.data(), data.size());
    }
    EXPECT_EQ(result, Generator::Result::OK);
    EXPECT_EQ(sink.size, fiber::util::base64_encoded_size(data.size()) + 2);
}

TEST(GeneratorTest, SetIndentStringKeepsIndentWhenOutOfMemory) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    const std::string long_indent(256, ' ');
    StringSink sink;
    Generator gen(sink);
    gen.set_option(Generator::Option::Beauty, true);
    bool set = true;
    {
        fiber::test::ScopedAllocFailure out_of_memory(0);
        set = gen.set_indent_string(long_indent);
    }
    EXPECT_FALSE(set);

    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);
    EXPECT_EQ(sink.output, "[\n    1\n]");
}

TEST(GeneratorTest, StringSinkAppends) {
    std::string output;
    fiber::json::StringSink sink(output);
    Generator gen(sink);
    EXPECT_EQ(gen.string("x", 1), Generator::Result::OK);
    EXPECT_EQ(output, "\"x\"");
    EXPECT_FALSE(sink.out_of_memory());
}

TEST(GeneratorTest, StringSinkReportsOutOfMemory) {
    if constexpr (!fiber::test::kAllocFailureAvailable) {
        GTEST_SKIP() << "no operator new replacement in this build";
    }
    const std::string payload(4096, 'x');
    std::string output;
    fiber::json::StringSink sink(output);
    Generator gen(sink);
    Generator::Result result = Generator::Result::OK;
    {
        fiber::test::ScopedAllocFailure out_of_memory(0);
        result = gen.string(payload);
    }
    EXPECT_EQ(result, Generator::Result::ErrorState);
    EXPECT_TRUE(sink.out_of_memory());
    EXPECT_EQ(gen.get_state(), Generator::State::Error);
}

TEST(GeneratorTest, Base64EncodeToMatchesRfc4648Vectors) {
    const std::pair<std::string_view, std::string_view> vectors[] = {
            {"", ""},
            {"f", "Zg=="},
            {"fo", "Zm8="},
            {"foo", "Zm9v"},
            {"foob", "Zm9vYg=="},
            {"fooba", "Zm9vYmE="},
            {"foobar", "Zm9vYmFy"},
    };
    for (const auto &[input, expected]: vectors) {
        const auto *data = reinterpret_cast<const std::uint8_t *>(input.data());
        ASSERT_EQ(fiber::util::base64_encoded_size(input.size()), expected.size()) << input;
        std::string out(expected.size(), '?');
        fiber::util::base64_encode_to(data, input.size(), out.data());
        EXPECT_EQ(out, expected) << input;
        EXPECT_EQ(fiber::util::base64_encode(data, input.size()), expected) << input;
    }
}
