#include <gtest/gtest.h>

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

} // namespace
