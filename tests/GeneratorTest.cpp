#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
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

TEST(GeneratorTest, StringPiecesEscapeAcrossPieces) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.string_begin(), Generator::Result::OK);
    EXPECT_EQ(gen.get_state(), Generator::State::InValueString);
    EXPECT_EQ(gen.string_append("a", 1), Generator::Result::OK);
    EXPECT_EQ(gen.string_append("\"b", 2), Generator::Result::OK);
    EXPECT_EQ(gen.string_append(nullptr, 0), Generator::Result::OK);
    EXPECT_EQ(gen.string_end(), Generator::Result::OK);
    EXPECT_EQ(gen.get_state(), Generator::State::Complete);
    EXPECT_EQ(sink.output, "\"a\\\"b\"");
}

TEST(GeneratorTest, StringPiecesWorkAsKeyAndValue) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.string_begin(), Generator::Result::OK);
    EXPECT_EQ(gen.get_state(), Generator::State::InKeyString);
    EXPECT_EQ(gen.string_append("ke", 2), Generator::Result::OK);
    EXPECT_EQ(gen.string_append("y", 1), Generator::Result::OK);
    EXPECT_EQ(gen.string_end(), Generator::Result::OK);
    EXPECT_EQ(gen.get_state(), Generator::State::MapValue);
    EXPECT_EQ(gen.string_begin(), Generator::Result::OK);
    EXPECT_EQ(gen.string_append("value", 5), Generator::Result::OK);
    EXPECT_EQ(gen.string_end(), Generator::Result::OK);
    EXPECT_EQ(gen.string("next", 4), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::OK);
    EXPECT_EQ(gen.map_close(), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"key\":\"value\",\"next\":1}");
}

TEST(GeneratorTest, StringPiecesInArrayKeepSeparators) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.array_open(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(1), Generator::Result::OK);
    EXPECT_EQ(gen.string_begin(), Generator::Result::OK);
    EXPECT_EQ(gen.string_end(), Generator::Result::OK);
    EXPECT_EQ(gen.integer(2), Generator::Result::OK);
    EXPECT_EQ(gen.array_close(), Generator::Result::OK);
    EXPECT_EQ(sink.output, "[1,\"\",2]");
}

TEST(GeneratorTest, OpenPieceRejectsEverythingElse) {
    using Call = Generator::Result (*)(Generator &);
    const Call calls[] = {
            [](Generator &gen) { return gen.integer(1); },     [](Generator &gen) { return gen.null_value(); },
            [](Generator &gen) { return gen.string("x", 1); }, [](Generator &gen) { return gen.string_begin(); },
            [](Generator &gen) { return gen.binary_begin(); }, [](Generator &gen) { return gen.map_open(); },
            [](Generator &gen) { return gen.array_open(); },   [](Generator &gen) { return gen.map_close(); },
            [](Generator &gen) { return gen.array_close(); },
    };
    for (const Call call: calls) {
        // Open inside a map key and inside an array, where the closes would otherwise pass.
        StringSink key_sink;
        Generator key_gen(key_sink);
        ASSERT_EQ(key_gen.map_open(), Generator::Result::OK);
        ASSERT_EQ(key_gen.string_begin(), Generator::Result::OK);
        EXPECT_EQ(call(key_gen), Generator::Result::ErrorState);
        EXPECT_EQ(key_gen.get_state(), Generator::State::Error);

        StringSink array_sink;
        Generator array_gen(array_sink);
        ASSERT_EQ(array_gen.array_open(), Generator::Result::OK);
        ASSERT_EQ(array_gen.binary_begin(), Generator::Result::OK);
        EXPECT_EQ(call(array_gen), Generator::Result::ErrorState);
        EXPECT_EQ(array_gen.get_state(), Generator::State::Error);
    }
}

TEST(GeneratorTest, PieceCallsNeedTheMatchingBegin) {
    const std::uint8_t byte = 1;
    {
        StringSink sink;
        Generator gen(sink);
        EXPECT_EQ(gen.string_append("x", 1), Generator::Result::ErrorState);
    }
    {
        StringSink sink;
        Generator gen(sink);
        EXPECT_EQ(gen.string_end(), Generator::Result::ErrorState);
    }
    {
        StringSink sink;
        Generator gen(sink);
        EXPECT_EQ(gen.binary_append(&byte, 1), Generator::Result::ErrorState);
    }
    {
        StringSink sink;
        Generator gen(sink);
        EXPECT_EQ(gen.binary_end(), Generator::Result::ErrorState);
    }
    {
        StringSink sink;
        Generator gen(sink);
        ASSERT_EQ(gen.string_begin(), Generator::Result::OK);
        EXPECT_EQ(gen.binary_end(), Generator::Result::ErrorState);
    }
    {
        StringSink sink;
        Generator gen(sink);
        ASSERT_EQ(gen.binary_begin(), Generator::Result::OK);
        EXPECT_EQ(gen.string_end(), Generator::Result::ErrorState);
    }
}

TEST(GeneratorTest, StringPiecesFitAtTheDepthLimit) {
    StringSink sink;
    Generator gen(sink);
    for (size_t i = 0; i + 1 < Generator::MAX_STACK; ++i) {
        ASSERT_EQ(gen.array_open(), Generator::Result::OK);
    }
    EXPECT_EQ(gen.array_open(), Generator::Result::MaxDepthExceeded);

    StringSink deep_sink;
    Generator deep(deep_sink);
    for (size_t i = 0; i + 1 < Generator::MAX_STACK; ++i) {
        ASSERT_EQ(deep.array_open(), Generator::Result::OK);
    }
    EXPECT_EQ(deep.string_begin(), Generator::Result::OK);
    EXPECT_EQ(deep.string_append("x", 1), Generator::Result::OK);
    EXPECT_EQ(deep.string_end(), Generator::Result::OK);
    EXPECT_EQ(deep.binary_begin(), Generator::Result::OK);
    EXPECT_EQ(deep.binary_end(), Generator::Result::OK);
    for (size_t i = 0; i + 1 < Generator::MAX_STACK; ++i) {
        ASSERT_EQ(deep.array_close(), Generator::Result::OK);
    }
    EXPECT_EQ(deep.get_state(), Generator::State::Complete);
}

TEST(GeneratorTest, StringPiecesValidateEachPiece) {
    StringSink sink;
    Generator gen(sink);
    gen.set_option(Generator::Option::ValidateUtf8, true);
    ASSERT_EQ(gen.string_begin(), Generator::Result::OK);
    EXPECT_EQ(gen.string_append("\xE4\xB8\xAD", 3), Generator::Result::OK);
    EXPECT_EQ(gen.string_append("\xE4\xB8", 2), Generator::Result::InvalidString);
    EXPECT_EQ(gen.get_state(), Generator::State::Error);
}

TEST(GeneratorTest, BinaryPiecesMatchOneCall) {
    const std::vector<std::uint8_t> data = patterned_bytes(4000);
    StringSink whole_sink;
    Generator whole(whole_sink);
    ASSERT_EQ(whole.binary(data.data(), data.size()), Generator::Result::OK);

    for (const size_t piece: {size_t{3}, size_t{768}, size_t{771}, size_t{3999}}) {
        StringSink sink;
        Generator gen(sink);
        ASSERT_EQ(gen.binary_begin(), Generator::Result::OK);
        for (size_t pos = 0; pos < data.size(); pos += piece) {
            const size_t len = std::min(piece, data.size() - pos);
            ASSERT_EQ(gen.binary_append(data.data() + pos, len), Generator::Result::OK);
        }
        ASSERT_EQ(gen.binary_end(), Generator::Result::OK);
        EXPECT_EQ(sink.output, whole_sink.output) << piece;
    }
}

TEST(GeneratorTest, BinaryPieceAfterPaddingFails) {
    const std::uint8_t bytes[] = {1, 2, 3, 4};
    StringSink sink;
    Generator gen(sink);
    ASSERT_EQ(gen.binary_begin(), Generator::Result::OK);
    EXPECT_EQ(gen.binary_append(bytes, 4), Generator::Result::OK);
    EXPECT_EQ(gen.get_state(), Generator::State::InBinaryTail);
    EXPECT_EQ(gen.binary_append(bytes, 3), Generator::Result::ErrorState);
}

TEST(GeneratorTest, BinaryAsMapKeyStillFails) {
    StringSink sink;
    Generator gen(sink);
    ASSERT_EQ(gen.map_open(), Generator::Result::OK);
    EXPECT_EQ(gen.binary_begin(), Generator::Result::KeysMustBeString);
}

TEST(GeneratorTest, BytesWrittenTracksTheSink) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(gen.bytes_written(), 0U);
    ASSERT_EQ(gen.array_open(), Generator::Result::OK);
    ASSERT_EQ(gen.string("a\nb", 3), Generator::Result::OK);
    ASSERT_EQ(gen.double_value(1.5), Generator::Result::OK);
    ASSERT_EQ(gen.binary(reinterpret_cast<const std::uint8_t *>("hi"), 2), Generator::Result::OK);
    ASSERT_EQ(gen.array_close(), Generator::Result::OK);
    EXPECT_EQ(gen.bytes_written(), sink.output.size());
    gen.clear();
    EXPECT_EQ(gen.bytes_written(), 0U);
}

TEST(GeneratorTest, EveryByteEscapesTheSameAtAnyPosition) {
    const auto escaped = [](unsigned char ch, bool escape_solidus) {
        switch (ch) {
            case '"':
                return std::string("\\\"");
            case '\\':
                return std::string("\\\\");
            case '\b':
                return std::string("\\b");
            case '\f':
                return std::string("\\f");
            case '\n':
                return std::string("\\n");
            case '\r':
                return std::string("\\r");
            case '\t':
                return std::string("\\t");
            case '/':
                return std::string(escape_solidus ? "\\/" : "/");
            default:
                break;
        }
        if (ch < 0x20) {
            std::string out = "\\u00";
            out.push_back("0123456789ABCDEF"[ch >> 4]);
            out.push_back("0123456789ABCDEF"[ch & 0x0F]);
            return out;
        }
        return std::string(1, static_cast<char>(ch));
    };

    for (const bool escape_solidus: {false, true}) {
        for (int value = 0; value < 256; ++value) {
            const auto ch = static_cast<unsigned char>(value);
            const std::string e = escaped(ch, escape_solidus);
            const char c = static_cast<char>(ch);
            // Alone, first, last and between plain runs.
            const std::pair<std::string, std::string> cases[] = {
                    {std::string(1, c), "\"" + e + "\""},
                    {std::string(1, c) + "ab", "\"" + e + "ab\""},
                    {"ab" + std::string(1, c), "\"ab" + e + "\""},
                    {std::string("ab").append(1, c).append("cd").append(1, c),
                     std::string("\"ab").append(e).append("cd").append(e).append("\"")},
            };
            for (const auto &[input, expected]: cases) {
                StringSink sink;
                Generator gen(sink);
                gen.set_option(Generator::Option::EscapeSolidus, escape_solidus);
                ASSERT_EQ(gen.string(input.data(), input.size()), Generator::Result::OK);
                EXPECT_EQ(sink.output, expected) << value;
            }
        }
    }
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
        std::string encoded(fiber::util::base64_encoded_size(len), '\0');
        fiber::util::base64_encode_into(data.data(), len, encoded.data());
        EXPECT_EQ(sink.output, "\"" + encoded + "\"") << len;
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
