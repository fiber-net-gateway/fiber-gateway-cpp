#include <gtest/gtest.h>

#include "common/json/JsGc.h"

namespace {

using fiber::json::GcArray;
using fiber::json::GcHeap;
using fiber::json::GcIterator;
using fiber::json::GcIteratorMode;
using fiber::json::GcObject;
using fiber::json::GcRootSet;
using fiber::json::GcString;
using fiber::json::GcStringEncoding;
using fiber::json::JsHeapKind;
using fiber::json::JsNodeType;
using fiber::json::JsValue;

TEST(JsGcTest, BytesIncludeExternalBuffers) {
    GcHeap heap;

    std::size_t base = fiber::json::gc_bytes_used(heap);
    GcString *str = fiber::json::gc_new_string_bytes(&heap, reinterpret_cast<const std::uint8_t *>("abc"), 3);
    ASSERT_NE(str, nullptr);
    base += fiber::json::gc_estimate_string_bytes(3, GcStringEncoding::Byte);
    EXPECT_EQ(fiber::json::gc_bytes_used(heap), base);

    GcArray *arr = fiber::json::gc_new_array(&heap, 4);
    ASSERT_NE(arr, nullptr);
    base += fiber::json::gc_estimate_array_bytes(4);
    EXPECT_EQ(fiber::json::gc_bytes_used(heap), base);

    GcObject *obj = fiber::json::gc_new_object(&heap, 2);
    ASSERT_NE(obj, nullptr);
    base += fiber::json::gc_estimate_object_bytes(2);
    EXPECT_EQ(fiber::json::gc_bytes_used(heap), base);
}

TEST(JsGcTest, IteratorSnapshotBytesAreAccounted) {
    GcHeap heap;

    GcObject *obj = fiber::json::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);
    GcString *key_a = fiber::json::gc_new_string(&heap, "a", 1);
    GcString *key_b = fiber::json::gc_new_string(&heap, "b", 1);
    GcString *key_c = fiber::json::gc_new_string(&heap, "c", 1);
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_c, nullptr);
    ASSERT_TRUE(fiber::json::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::json::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));

    GcIterator *iter = fiber::json::gc_new_object_iterator(&heap, obj, GcIteratorMode::Keys);
    ASSERT_NE(iter, nullptr);

    JsValue out;
    bool done = false;
    ASSERT_TRUE(fiber::json::gc_iterator_next(&heap, iter, out, done));
    ASSERT_FALSE(done);

    std::size_t before_snapshot = fiber::json::gc_bytes_used(heap);
    ASSERT_TRUE(fiber::json::gc_object_set(&heap, obj, key_c, JsValue::make_integer(3)));
    ASSERT_TRUE(fiber::json::gc_iterator_next(&heap, iter, out, done));
    ASSERT_FALSE(done);

    EXPECT_EQ(fiber::json::gc_bytes_used(heap), before_snapshot + fiber::json::gc_estimate_object_snapshot_bytes(2));
}

class SingleValueProvider final : public GcRootSet::RootProvider {
public:
    explicit SingleValueProvider(JsValue &value) : value_(&value) {}

    void visit_roots(GcRootSet::RootVisitor &visitor) override { visitor.visit(value_); }

private:
    JsValue *value_ = nullptr;
};

TEST(JsGcTest, RootProvidersMarkValuesWithoutTemporaryRootVector) {
    GcHeap heap;
    GcRootSet roots;

    JsValue rooted = JsValue::make_undefined();
    GcString *live = fiber::json::gc_new_string(&heap, "live", 4);
    GcString *garbage = fiber::json::gc_new_string(&heap, "dead", 4);
    ASSERT_NE(live, nullptr);
    ASSERT_NE(garbage, nullptr);
    rooted = js_make_heap_ref(&live->hdr, JsHeapKind::String);

    SingleValueProvider provider(rooted);
    roots.add_provider(&provider);

    std::size_t before_collect = fiber::json::gc_bytes_used(heap);
    fiber::json::gc_collect(heap, roots);
    fiber::json::gc_collect(heap, roots);
    std::size_t after_collect = fiber::json::gc_bytes_used(heap);

    EXPECT_EQ(js_value_type(rooted), JsNodeType::String);
    EXPECT_EQ(js_value_heap_header(rooted), &live->hdr);
    EXPECT_LT(after_collect, before_collect);
    EXPECT_EQ(after_collect, fiber::json::gc_estimate_string_bytes(4, GcStringEncoding::Byte));
}

} // namespace
