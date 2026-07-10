#include <gtest/gtest.h>

#include <string_view>

#include "script/gc/Wtf8.h"

namespace {

using fiber::script::gc_detail::Wtf8Cursor;
using fiber::script::gc_detail::Wtf8MeasureResult;

TEST(Wtf8Test, RoundTripsEverySingleUtf16Unit) {
    for (std::uint32_t value = 0; value <= 0xFFFF; ++value) {
        const char16_t input = static_cast<char16_t>(value);
        Wtf8MeasureResult measure;
        ASSERT_TRUE(fiber::script::gc_detail::wtf8_measure_utf16(&input, 1, measure));
        ASSERT_GE(measure.byte_len, 1u);
        ASSERT_LE(measure.byte_len, 3u);

        char encoded[3] = {};
        ASSERT_TRUE(fiber::script::gc_detail::wtf8_write_utf16(&input, 1, encoded, measure.byte_len));
        Wtf8Cursor cursor{.data = encoded, .len = measure.byte_len};
        char16_t decoded = 0;
        ASSERT_TRUE(fiber::script::gc_detail::wtf8_next_utf16_unit(cursor, decoded));
        EXPECT_EQ(decoded, input);
        EXPECT_FALSE(fiber::script::gc_detail::wtf8_next_utf16_unit(cursor, decoded));
        EXPECT_FALSE(cursor.malformed);

        const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
        EXPECT_EQ(measure.well_formed, !surrogate);
        EXPECT_EQ(fiber::script::gc_detail::wtf8_is_well_formed(encoded, measure.byte_len), !surrogate);
    }
}

TEST(Wtf8Test, SurrogatePairUsesCanonicalFourByteUtf8) {
    const char16_t input[] = {static_cast<char16_t>(0xD83D), static_cast<char16_t>(0xDE00)};
    Wtf8MeasureResult measure;
    ASSERT_TRUE(fiber::script::gc_detail::wtf8_measure_utf16(input, 2, measure));
    EXPECT_EQ(measure.byte_len, 4u);
    EXPECT_TRUE(measure.well_formed);

    char encoded[4] = {};
    ASSERT_TRUE(fiber::script::gc_detail::wtf8_write_utf16(input, 2, encoded, sizeof(encoded)));
    EXPECT_EQ(std::string_view(encoded, sizeof(encoded)), "\xF0\x9F\x98\x80");
    EXPECT_TRUE(fiber::script::gc_detail::wtf8_is_well_formed(encoded, sizeof(encoded)));
}

} // namespace
