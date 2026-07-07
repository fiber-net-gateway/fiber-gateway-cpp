#include <gtest/gtest.h>

#include <string>

#include "script/JsGc.h"

namespace {

using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcIterator;
using fiber::script::GcIteratorMode;
using fiber::script::GcObject;
using fiber::script::GcRootRegistration;
using fiber::script::GcRootSource;
using fiber::script::GcRootVisitor;
using fiber::script::GcString;
using fiber::script::JsHeapKind;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ValueHandle;

static_assert(noexcept(fiber::script::gc_new_string(nullptr, nullptr, 0)));

std::string to_utf8(const GcString *str) {
    std::string out;
    EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    return out;
}

TEST(JsGcTest, BytesIncludeExternalBuffers) {
    GcHeap heap;

    std::size_t base = heap.bytes;
    GcString *short_str = fiber::script::gc_new_string_bytes(&heap, reinterpret_cast<const std::uint8_t *>("abc"), 3);
    ASSERT_NE(short_str, nullptr);
    std::size_t after_short = heap.bytes;
    EXPECT_GT(after_short, base);

    GcString *long_str =
            fiber::script::gc_new_string_bytes(&heap, reinterpret_cast<const std::uint8_t *>("0123456789"), 10);
    ASSERT_NE(long_str, nullptr);
    std::size_t after_long = heap.bytes;
    // External data buffer is accounted: the longer string claims strictly more bytes.
    EXPECT_GT(after_long - after_short, after_short - base);

    GcArray *arr = fiber::script::gc_new_array(&heap, 4);
    ASSERT_NE(arr, nullptr);
    std::size_t after_array = heap.bytes;
    EXPECT_GT(after_array, after_long);

    GcObject *obj = fiber::script::gc_new_object(&heap, 2);
    ASSERT_NE(obj, nullptr);
    EXPECT_GT(heap.bytes, after_array);
}

TEST(JsGcTest, IteratorSnapshotBytesAreAccounted) {
    GcHeap heap;

    GcObject *obj = fiber::script::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);
    GcString *key_a = fiber::script::gc_new_string(&heap, "a", 1);
    GcString *key_b = fiber::script::gc_new_string(&heap, "b", 1);
    GcString *key_c = fiber::script::gc_new_string(&heap, "c", 1);
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_c, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));

    GcIterator *iter = fiber::script::gc_new_object_iterator(&heap, obj, GcIteratorMode::Keys);
    ASSERT_NE(iter, nullptr);

    JsValue out;
    bool done = false;
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    ASSERT_FALSE(done);

    std::size_t before_snapshot = heap.bytes;
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_c, JsValue::make_integer(3)));
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    ASSERT_FALSE(done);

    // The version mismatch forces a snapshot of the two original keys.
    EXPECT_EQ(heap.bytes - before_snapshot, 2u * sizeof(GcString *));
}

class SingleValueSource final : public GcRootSource {
public:
    explicit SingleValueSource(JsValue &value) : value_(&value) {}

    void visit_roots(GcRootVisitor &visitor) noexcept override { visitor.visit(value_); }

private:
    JsValue *value_ = nullptr;
};

TEST(JsGcTest, RootSourcesMarkValuesWithoutTemporaryRootVector) {
    GcHeap heap;

    JsValue rooted = JsValue::make_undefined();
    GcString *live = fiber::script::gc_new_string(&heap, "live", 4);
    GcString *garbage = fiber::script::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(live, nullptr);
    ASSERT_NE(garbage, nullptr);
    rooted = js_make_heap_ref(&live->hdr, JsHeapKind::String);

    SingleValueSource source(rooted);
    GcRootRegistration reg(heap.roots(), source);

    std::size_t before_collect = heap.bytes;
    heap.collect();
    heap.collect();
    std::size_t after_collect = heap.bytes;

    EXPECT_EQ(js_value_type(rooted), JsNodeType::String);
    EXPECT_EQ(js_value_heap_header(rooted), &live->hdr);
    EXPECT_LT(after_collect, before_collect);
    EXPECT_GT(after_collect, 0u);
    // The live set is stable: a follow-up collect reclaims nothing.
    EXPECT_EQ(heap.collect().freed, 0u);
}

TEST(JsGcTest, GcHeapGlobalSlotsAreCollectedAsRoots) {
    GcHeap heap;

    GcString *live = fiber::script::gc_new_string(&heap, "live", 4);
    GcString *garbage = fiber::script::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(live, nullptr);
    ASSERT_NE(garbage, nullptr);

    ValueHandle root = heap.global_value();
    ASSERT_NE(root, nullptr);
    *root = js_make_heap_ref(&live->hdr, JsHeapKind::String);

    std::size_t before_collect = heap.bytes;
    heap.collect();
    auto second = heap.collect();

    EXPECT_EQ(js_value_heap_header(*root), &live->hdr);
    EXPECT_LT(heap.bytes, before_collect);
    EXPECT_GT(heap.bytes, 0u);
    // Freshly allocated objects are pre-marked, so the unreachable one survives the first collect
    // and is only reclaimed on the second.
    EXPECT_GT(second.freed, 0u);
}

TEST(JsGcTest, GcHeapLocalMarkReleasesLocalRoots) {
    GcHeap heap;

    {
        GcHeap::LocalMark mark(heap);
        GcString *live = fiber::script::gc_new_string(&heap, "live", 4);
        ASSERT_NE(live, nullptr);
        ValueHandle root = heap.local_value();
        ASSERT_NE(root, nullptr);
        *root = js_make_heap_ref(&live->hdr, JsHeapKind::String);

        heap.collect();
        EXPECT_EQ(js_value_heap_header(*root), &live->hdr);
        EXPECT_GT(heap.bytes, 0u);
    }

    // Local roots are released when LocalMark drops; a collect now sweeps the live string.
    heap.collect();
    EXPECT_EQ(heap.bytes, 0u);
}

TEST(JsGcTest, NewStringDecodesUtf8IntoCompactStorage) {
    GcHeap heap;

    GcString *ascii = fiber::script::gc_new_string(&heap, "abc", 3);
    ASSERT_NE(ascii, nullptr);
    ASSERT_EQ(ascii->encoding, fiber::script::GcStringEncoding::Byte);
    ASSERT_EQ(ascii->len, 3u);
    EXPECT_EQ(to_utf8(ascii), "abc");

    const char latin1[] = {static_cast<char>(0xC3), static_cast<char>(0xA9)};
    GcString *latin1_str = fiber::script::gc_new_string(&heap, latin1, sizeof(latin1));
    ASSERT_NE(latin1_str, nullptr);
    ASSERT_EQ(latin1_str->encoding, fiber::script::GcStringEncoding::Byte);
    ASSERT_EQ(latin1_str->len, 1u);
    ASSERT_NE(latin1_str->data8, nullptr);
    EXPECT_EQ(latin1_str->data8[0], 0xE9u);
    EXPECT_EQ(to_utf8(latin1_str), std::string(latin1, sizeof(latin1)));

    const char emoji[] = {static_cast<char>(0xF0), static_cast<char>(0x9F), static_cast<char>(0x98),
                          static_cast<char>(0x80)};
    GcString *emoji_str = fiber::script::gc_new_string(&heap, emoji, sizeof(emoji));
    ASSERT_NE(emoji_str, nullptr);
    ASSERT_EQ(emoji_str->encoding, fiber::script::GcStringEncoding::Utf16);
    ASSERT_EQ(emoji_str->len, 2u);
    ASSERT_NE(emoji_str->data16, nullptr);
    EXPECT_EQ(emoji_str->data16[0], static_cast<char16_t>(0xD83D));
    EXPECT_EQ(emoji_str->data16[1], static_cast<char16_t>(0xDE00));
    EXPECT_EQ(to_utf8(emoji_str), std::string(emoji, sizeof(emoji)));
}

TEST(JsGcTest, NewStringRejectsMalformedUtf8) {
    GcHeap heap;

    const char overlong[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    EXPECT_EQ(fiber::script::gc_new_string(&heap, overlong, sizeof(overlong)), nullptr);

    const char truncated[] = {static_cast<char>(0xE2), static_cast<char>(0x82)};
    EXPECT_EQ(fiber::script::gc_new_string(&heap, truncated, sizeof(truncated)), nullptr);

    const char surrogate[] = {static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80)};
    EXPECT_EQ(fiber::script::gc_new_string(&heap, surrogate, sizeof(surrogate)), nullptr);
}

} // namespace
