#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
#include "http/Http2HpackDiscardSink.h"
#include "http/Huffman.h"

namespace {
using namespace fiber;
common::IoErr decode(http::Http2HpackDecoder &decoder, std::string_view block) {
    return decoder.decode(reinterpret_cast<const std::uint8_t *>(block.data()), block.size(), true);
}
} // namespace

TEST(Http2HpackDiscardSinkTest, DiscardedLiteralIsAvailableToNextHeaderBlock) {
    http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    http::Http2HpackDiscardSink sink(65536);
    decoder.begin_block(&sink, &sink.ops());
    const std::string literal = std::string("\x40\x06x-test\x05value", 14);
    for (std::size_t i = 0; i < literal.size(); ++i) {
        ASSERT_EQ(
                decoder.decode(reinterpret_cast<const std::uint8_t *>(literal.data() + i), 1, i + 1 == literal.size()),
                common::IoErr::None);
    }
    sink.finish_block();
    auto ops = sink.ops();
    ops.on_indexed_field = [](void *, http::Http2HpackDecoder::TableEntryView field) noexcept {
        EXPECT_EQ(field.name, "x-test");
        EXPECT_EQ(field.value, "value");
        return common::IoErr::None;
    };
    decoder.begin_block(&sink, &ops);
    EXPECT_EQ(decode(decoder, "\xbe"), common::IoErr::None);
}

TEST(Http2HpackDiscardSinkTest, IndexedNameAndHuffmanValueUpdateDynamicTable) {
    http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    http::Http2HpackDiscardSink sink(65536);
    decoder.begin_block(&sink, &sink.ops());
    // RFC HPACK example: :authority: www.example.com, Huffman encoded.
    const unsigned char bytes[]{0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff};
    ASSERT_EQ(decoder.decode(bytes, sizeof(bytes), true), common::IoErr::None);
    auto ops = sink.ops();
    ops.on_indexed_field = [](void *, http::Http2HpackDecoder::TableEntryView field) noexcept {
        EXPECT_EQ(field.name, ":authority");
        EXPECT_EQ(field.value, "www.example.com");
        return common::IoErr::None;
    };
    decoder.begin_block(&sink, &ops);
    EXPECT_EQ(decode(decoder, "\xbe"), common::IoErr::None);
}

// An empty value is legal HPACK and reaches the sink with a zero-length
// string. The buffer it materializes into must still be backed: the decoder
// indexes the field from the FieldView the sink fills in.
TEST(Http2HpackDiscardSinkTest, EmptyNameAndValueAreIndexedWithoutCrashing) {
    http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    http::Http2HpackDiscardSink sink(65536);
    decoder.begin_block(&sink, &sink.ops());
    // Literal with incremental indexing: ("x-empty", "") then ("", "v").
    ASSERT_EQ(decode(decoder, std::string("\x40\x07x-empty\x00\x40\x00\x01v", 14)), common::IoErr::None);
    sink.finish_block();

    struct Seen {
        std::vector<std::pair<std::string, std::string>> fields;
    } seen;
    auto ops = sink.ops();
    ops.on_indexed_field = [](void *ctx, http::Http2HpackDecoder::TableEntryView field) noexcept {
        static_cast<Seen *>(ctx)->fields.emplace_back(field.name, field.value);
        return common::IoErr::None;
    };
    // Newest entry is index 62, so ("", "v") comes back before ("x-empty", "").
    decoder.begin_block(&seen, &ops);
    EXPECT_EQ(decode(decoder, "\xbe\xbf"), common::IoErr::None);
    ASSERT_EQ(seen.fields.size(), 2u);
    EXPECT_EQ(seen.fields[0], std::make_pair(std::string(""), std::string("v")));
    EXPECT_EQ(seen.fields[1], std::make_pair(std::string("x-empty"), std::string("")));
}

TEST(Http2HpackDiscardSinkTest, RejectsInvalidHuffmanAndDecodedSizeLimit) {
    http::Http2HpackDecoder decoder;
    ASSERT_TRUE(decoder.init());
    http::Http2HpackDiscardSink sink(4);
    decoder.begin_block(&sink, &sink.ops());
    const unsigned char invalid[]{0x01, 0x81, 0xff};
    EXPECT_EQ(decoder.decode(invalid, sizeof(invalid), true), common::IoErr::Invalid);
    decoder.begin_block(&sink, &sink.ops());
    EXPECT_EQ(decode(decoder, std::string("\x00\x05hello\x00", 8)), common::IoErr::Invalid);
}
