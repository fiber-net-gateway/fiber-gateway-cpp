#include <gtest/gtest.h>

#include "script/JsGc.h"

namespace {

using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcIterator;
using fiber::script::GcIteratorMode;
using fiber::script::GcObject;
using fiber::script::GcRootRegistration;
using fiber::script::GcRootSet;
using fiber::script::GcRootSource;
using fiber::script::GcRootVisitor;
using fiber::script::GcString;
using fiber::script::GcStringEncoding;
using fiber::script::JsHeapKind;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ValueHandle;

TEST(JsGcTest, BytesIncludeExternalBuffers) {
    GcHeap heap;

    std::size_t base = fiber::script::gc_bytes_used(heap);
    GcString *str = fiber::script::gc_new_string_bytes(&heap, reinterpret_cast<const std::uint8_t *>("abc"), 3);
    ASSERT_NE(str, nullptr);
    base += fiber::script::gc_estimate_string_bytes(3, GcStringEncoding::Byte);
    EXPECT_EQ(fiber::script::gc_bytes_used(heap), base);

    GcArray *arr = fiber::script::gc_new_array(&heap, 4);
    ASSERT_NE(arr, nullptr);
    base += fiber::script::gc_estimate_array_bytes(4);
    EXPECT_EQ(fiber::script::gc_bytes_used(heap), base);

    GcObject *obj = fiber::script::gc_new_object(&heap, 2);
    ASSERT_NE(obj, nullptr);
    base += fiber::script::gc_estimate_object_bytes(2);
    EXPECT_EQ(fiber::script::gc_bytes_used(heap), base);
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

    std::size_t before_snapshot = fiber::script::gc_bytes_used(heap);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_c, JsValue::make_integer(3)));
    ASSERT_TRUE(fiber::script::gc_iterator_next(&heap, iter, out, done));
    ASSERT_FALSE(done);

    EXPECT_EQ(fiber::script::gc_bytes_used(heap),
              before_snapshot + fiber::script::gc_estimate_object_snapshot_bytes(2));
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
    GcRootSet roots;

    JsValue rooted = JsValue::make_undefined();
    GcString *live = fiber::script::gc_new_string(&heap, "live", 4);
    GcString *garbage = fiber::script::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(live, nullptr);
    ASSERT_NE(garbage, nullptr);
    rooted = js_make_heap_ref(&live->hdr, JsHeapKind::String);

    SingleValueSource source(rooted);
    GcRootRegistration reg(roots, source);

    std::size_t before_collect = fiber::script::gc_bytes_used(heap);
    fiber::script::gc_collect(heap, roots);
    fiber::script::gc_collect(heap, roots);
    std::size_t after_collect = fiber::script::gc_bytes_used(heap);

    EXPECT_EQ(js_value_type(rooted), JsNodeType::String);
    EXPECT_EQ(js_value_heap_header(rooted), &live->hdr);
    EXPECT_LT(after_collect, before_collect);
    EXPECT_EQ(after_collect, fiber::script::gc_estimate_string_bytes(4, GcStringEncoding::Byte));
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

    std::size_t before_collect = fiber::script::gc_bytes_used(heap);
    heap.collect_now();
    heap.collect_now();

    EXPECT_EQ(js_value_heap_header(*root), &live->hdr);
    EXPECT_LT(fiber::script::gc_bytes_used(heap), before_collect);
    EXPECT_EQ(fiber::script::gc_bytes_used(heap), fiber::script::gc_estimate_string_bytes(4, GcStringEncoding::Byte));
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

        heap.collect_now();
        EXPECT_EQ(js_value_heap_header(*root), &live->hdr);
        EXPECT_EQ(fiber::script::gc_bytes_used(heap),
                  fiber::script::gc_estimate_string_bytes(4, GcStringEncoding::Byte));
    }

    heap.collect_now();
    EXPECT_EQ(fiber::script::gc_bytes_used(heap), 0u);
}

} // namespace
