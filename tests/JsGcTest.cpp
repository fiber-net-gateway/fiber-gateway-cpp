#include <gtest/gtest.h>

#include <string>

#include "script/JsGc.h"

namespace {

using fiber::script::GcArray;
using fiber::script::GcException;
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

TEST(JsGcTest, NewStringSurvivesFirstCollectAfterInternalBufferCollection) {
    GcHeap heap;
    heap.threshold = 1;

    std::string payload(1 << 20, 'x');
    GcString *str = fiber::script::gc_new_string(&heap, payload.data(), payload.size());
    ASSERT_NE(str, nullptr);
    ASSERT_GT(heap.bytes, payload.size());

    EXPECT_EQ(heap.collect().freed, 0u);
    auto second = heap.collect();
    EXPECT_GT(second.freed, 0u);
    EXPECT_EQ(heap.bytes, 0u);
}

TEST(JsGcTest, NoGcScopeDefersThresholdCollectionUntilExit) {
    GcHeap heap;

    GcString *garbage = fiber::script::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(garbage, nullptr);
    heap.collect();
    std::size_t old_bytes = heap.bytes;
    ASSERT_GT(old_bytes, 0u);

    heap.threshold = 1;
    std::size_t during_scope = 0;
    {
        GcHeap::NoGcScope no_gc(heap);
        std::string payload(1 << 20, 'x');
        GcString *fresh = fiber::script::gc_new_string(&heap, payload.data(), payload.size());
        ASSERT_NE(fresh, nullptr);
        EXPECT_TRUE(heap.no_gc_active());
        EXPECT_GT(heap.bytes, old_bytes);
        during_scope = heap.bytes;
    }

    EXPECT_FALSE(heap.no_gc_active());
    EXPECT_LT(heap.bytes, during_scope);
    EXPECT_GT(heap.bytes, 0u);
}

TEST(JsGcTest, NestedNoGcScopeCollectsOnlyAfterOuterExit) {
    GcHeap heap;

    GcString *garbage = fiber::script::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(garbage, nullptr);
    heap.collect();
    std::size_t old_bytes = heap.bytes;
    ASSERT_GT(old_bytes, 0u);

    heap.threshold = 1;
    std::size_t after_inner_exit = 0;
    {
        GcHeap::NoGcScope outer(heap);
        {
            GcHeap::NoGcScope inner(heap);
            GcString *fresh = fiber::script::gc_new_string(&heap, "fresh", 5);
            ASSERT_NE(fresh, nullptr);
        }
        EXPECT_TRUE(heap.no_gc_active());
        after_inner_exit = heap.bytes;
        EXPECT_GT(after_inner_exit, old_bytes);
    }

    EXPECT_FALSE(heap.no_gc_active());
    EXPECT_LT(heap.bytes, after_inner_exit);
    EXPECT_GT(heap.bytes, 0u);
}

TEST(JsGcTest, NoGcScopeDefersExplicitCollectUntilExit) {
    GcHeap heap;

    GcString *garbage = fiber::script::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(garbage, nullptr);
    heap.collect();
    std::size_t before_scope = heap.bytes;
    ASSERT_GT(before_scope, 0u);

    {
        GcHeap::NoGcScope no_gc(heap);
        auto stats = heap.collect();
        EXPECT_EQ(stats.total, before_scope);
        EXPECT_EQ(stats.freed, 0u);
        EXPECT_EQ(heap.bytes, before_scope);
    }

    EXPECT_EQ(heap.bytes, 0u);
}

TEST(JsGcTest, ValueApiBuildsObjectWithNativeKeyUnderLowThreshold) {
    GcHeap heap;
    heap.threshold = 1;

    ValueHandle obj = heap.global_value();
    ValueHandle out = heap.global_value();
    ASSERT_NE(obj, nullptr);
    ASSERT_NE(out, nullptr);

    ASSERT_TRUE(fiber::script::gc_make_object(&heap, obj, 0));
    ASSERT_TRUE(fiber::script::gc_object_set_key(&heap, obj, "answer", 6, JsValue::make_integer(42)));
    ASSERT_TRUE(fiber::script::gc_object_get_key(&heap, obj, "answer", 6, out));

    EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(*out), 42);
}

TEST(JsGcTest, ValueApiArrayKeepsPushedHeapValueAlive) {
    GcHeap heap;
    heap.threshold = 1;

    ValueHandle arr = heap.global_value();
    ValueHandle str = heap.global_value();
    ValueHandle out = heap.global_value();
    ASSERT_NE(arr, nullptr);
    ASSERT_NE(str, nullptr);
    ASSERT_NE(out, nullptr);

    ASSERT_TRUE(fiber::script::gc_make_array(&heap, arr, 0));
    ASSERT_TRUE(fiber::script::gc_make_string(&heap, str, "value", 5));
    ASSERT_TRUE(fiber::script::gc_array_push(&heap, arr, *str));
    *str = JsValue::make_undefined();

    heap.collect();
    ASSERT_TRUE(fiber::script::gc_array_get(arr, 0, out));

    std::string utf8;
    ASSERT_TRUE(fiber::script::gc_string_to_utf8(out, utf8));
    EXPECT_EQ(utf8, "value");
}

TEST(JsGcTest, ValueApiIteratorNextRootsEntryArray) {
    GcHeap heap;
    heap.threshold = 1;

    ValueHandle arr = heap.global_value();
    ValueHandle iter = heap.global_value();
    ValueHandle entry = heap.global_value();
    ValueHandle value = heap.global_value();
    ASSERT_NE(arr, nullptr);
    ASSERT_NE(iter, nullptr);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(value, nullptr);

    ASSERT_TRUE(fiber::script::gc_make_array(&heap, arr, 0));
    ASSERT_TRUE(fiber::script::gc_array_push(&heap, arr, JsValue::make_integer(9)));
    ASSERT_TRUE(fiber::script::gc_make_array_iterator(&heap, iter, arr, GcIteratorMode::Entries));

    bool done = true;
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, entry, done));
    ASSERT_FALSE(done);
    ASSERT_EQ(fiber::script::js_value_type(*entry), JsNodeType::Array);
    ASSERT_TRUE(fiber::script::gc_array_get(entry, 1, value));
    EXPECT_EQ(fiber::script::js_value_int64(*value), 9);
}

TEST(JsGcTest, NewExceptionRootsHeapInputsDuringAllocationCollection) {
    GcHeap heap;

    GcString *name = nullptr;
    GcString *message = nullptr;
    GcObject *meta_obj = nullptr;
    std::size_t rooted_bytes = 0;
    {
        GcHeap::LocalMark mark(heap);
        ValueHandle name_root = heap.local_value();
        ValueHandle message_root = heap.local_value();
        ValueHandle meta_root = heap.local_value();
        ASSERT_NE(name_root, nullptr);
        ASSERT_NE(message_root, nullptr);
        ASSERT_NE(meta_root, nullptr);

        name = fiber::script::gc_new_string(&heap, "TypeError", 9);
        message = fiber::script::gc_new_string(&heap, "boom", 4);
        meta_obj = fiber::script::gc_new_object(&heap, 0);
        ASSERT_NE(name, nullptr);
        ASSERT_NE(message, nullptr);
        ASSERT_NE(meta_obj, nullptr);
        *name_root = js_make_heap_ref(&name->hdr, JsHeapKind::String);
        *message_root = js_make_heap_ref(&message->hdr, JsHeapKind::String);
        *meta_root = js_make_heap_ref(&meta_obj->hdr, JsHeapKind::Object);

        heap.collect();
        rooted_bytes = heap.bytes;
    }

    heap.threshold = 1;
    JsValue meta = js_make_heap_ref(&meta_obj->hdr, JsHeapKind::Object);
    GcException *exc = fiber::script::gc_new_exception(&heap, 12, name, message, meta);
    ASSERT_NE(exc, nullptr);
    ASSERT_GE(heap.bytes, rooted_bytes + sizeof(GcException));
    EXPECT_EQ(exc->name, name);
    EXPECT_EQ(exc->message, message);
    EXPECT_EQ(fiber::script::js_value_heap_header(exc->meta), &meta_obj->hdr);
    EXPECT_EQ(to_utf8(exc->name), "TypeError");
    EXPECT_EQ(to_utf8(exc->message), "boom");
}

TEST(JsGcTest, NewExceptionFromNativeMetaRootsMetaDuringAllocationCollection) {
    GcHeap heap;

    GcObject *meta_obj = nullptr;
    std::size_t rooted_bytes = 0;
    {
        GcHeap::LocalMark mark(heap);
        ValueHandle meta_root = heap.local_value();
        ASSERT_NE(meta_root, nullptr);
        meta_obj = fiber::script::gc_new_object(&heap, 0);
        ASSERT_NE(meta_obj, nullptr);
        *meta_root = js_make_heap_ref(&meta_obj->hdr, JsHeapKind::Object);
        heap.collect();
        rooted_bytes = heap.bytes;
    }

    heap.threshold = 1;
    JsValue meta = js_make_heap_ref(&meta_obj->hdr, JsHeapKind::Object);
    GcException *exc = fiber::script::gc_new_exception(&heap, 18, nullptr, 0, nullptr, 0, meta);
    ASSERT_NE(exc, nullptr);
    ASSERT_GE(heap.bytes, rooted_bytes + sizeof(GcException));
    EXPECT_EQ(fiber::script::js_value_heap_header(exc->meta), &meta_obj->hdr);
}

TEST(JsGcTest, NewExceptionFromNativeStringsRootsTemporaryStrings) {
    GcHeap heap;
    heap.threshold = 1;

    std::string name(1 << 20, 'n');
    std::string message(1 << 20, 'm');
    GcException *exc =
            fiber::script::gc_new_exception(&heap, 21, name.data(), name.size(), message.data(), message.size());
    ASSERT_NE(exc, nullptr);
    ASSERT_GT(heap.bytes, name.size() + message.size());
    ASSERT_NE(exc->name, nullptr);
    ASSERT_NE(exc->message, nullptr);
    EXPECT_EQ(exc->name->len, name.size());
    EXPECT_EQ(exc->message->len, message.size());
    EXPECT_EQ(exc->name->data8[0], static_cast<std::uint8_t>('n'));
    EXPECT_EQ(exc->message->data8[0], static_cast<std::uint8_t>('m'));
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
