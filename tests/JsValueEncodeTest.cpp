#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueEncode.h>

using fiber::json::Generator;
using fiber::json::OutputSink;
using fiber::script::GcArray;
using fiber::script::GcException;
using fiber::script::GcHeap;
using fiber::script::GcHeapKind;
using fiber::script::GcObject;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::json::JsValueEncoder;

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

GcString *make_key(GcHeap &heap, const char *data) {
    return fiber::script::gc_new_string(&heap, data, std::strlen(data));
}

JsValue heap_string(GcHeap &heap, std::string_view text) {
    return JsValue::make_string(heap, text.data(), text.size());
}

JsValue array_of(GcHeap &heap, const std::vector<JsValue> &values) {
    GcArray *arr = fiber::script::gc_new_array(&heap, values.size());
    EXPECT_NE(arr, nullptr);
    std::copy(values.begin(), values.end(), arr->elems);
    arr->size = values.size();
    return js_make_heap_ref(&arr->hdr, GcHeapKind::Array);
}

std::string encode_whole(const JsValue &value) {
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, value), Generator::Result::OK);
    return sink.output;
}

// Encodes in run(limit) steps. Fails the test if a step passes the documented overshoot.
std::string encode_in_steps(const JsValue &value, std::size_t limit) {
    constexpr std::size_t kMaxOvershoot = std::size_t{96} * 1024;
    StringSink sink;
    Generator gen(sink);
    JsValueEncoder encoder(gen, value);
    for (;;) {
        const std::size_t before = sink.output.size();
        const JsValueEncoder::Status status = encoder.run(limit);
        EXPECT_NE(status, JsValueEncoder::Status::Failed);
        EXPECT_LE(sink.output.size() - before, limit + kMaxOvershoot);
        if (status != JsValueEncoder::Status::Yield) {
            return sink.output;
        }
    }
}

// Every kind of value, with long strings and binaries that have to go out in slices.
JsValue mixed_document(GcHeap &heap, const std::string &borrowed, const std::vector<std::uint8_t> &borrowed_bytes) {
    std::string chinese;
    for (int i = 0; i < 30000; ++i) {
        chinese += "\xE4\xB8\xAD";
    }
    std::string escapes(70000, '\n');
    std::string long_key(40000, 'k');
    std::vector<std::uint8_t> bytes(50000);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 31);
    }

    GcObject *obj = fiber::script::gc_new_object(&heap, 8);
    EXPECT_NE(obj, nullptr);
    const auto set = [&](std::string_view key, const JsValue &value) {
        GcString *name = fiber::script::gc_new_string(&heap, key.data(), key.size());
        ASSERT_NE(name, nullptr);
        ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, name, value));
    };
    set("chinese", heap_string(heap, chinese));
    set("escapes", heap_string(heap, escapes));
    set(long_key, JsValue::make_integer(1));
    set("binary", JsValue::make_binary(heap, bytes.data(), bytes.size()));
    set("borrowed", JsValue::make_native_string(borrowed.data(), borrowed.size()));
    set("borrowed_binary", JsValue::make_native_binary(borrowed_bytes.data(), borrowed_bytes.size()));
    set("empty_array", JsValue::make_array(heap, 0));
    set("empty_object", JsValue::make_object(heap, 0));

    GcException *exc = fiber::script::gc_new_exception(&heap, 7, "Name", 4, chinese.data(), chinese.size(),
                                                       array_of(heap, {JsValue::make_null()}));
    EXPECT_NE(exc, nullptr);
    const JsValue exception = js_make_heap_ref(&exc->hdr, GcHeapKind::Exception);

    const JsValue nested = array_of(heap, {array_of(heap, {array_of(heap, {JsValue::make_integer(1)})}), exception});
    set("nested", nested);
    return array_of(heap, {JsValue::make_null(), JsValue::make_undefined(), JsValue::make_boolean(true),
                           JsValue::make_integer(-5), JsValue::make_float(1.5),
                           js_make_heap_ref(&obj->hdr, GcHeapKind::Object), heap_string(heap, "tail")});
}

} // namespace

TEST(JsValueEncodeTest, EncodeObjectOrderAndOverwrite) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 4);
    ASSERT_NE(obj, nullptr);

    GcString *key_a = make_key(heap, "a");
    GcString *key_b = make_key(heap, "b");
    GcString *key_a2 = make_key(heap, "a");
    ASSERT_NE(key_a, nullptr);
    ASSERT_NE(key_b, nullptr);
    ASSERT_NE(key_a2, nullptr);

    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a, JsValue::make_integer(1)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_b, JsValue::make_integer(2)));
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, key_a2, JsValue::make_integer(3)));

    JsValue root = js_make_heap_ref(&obj->hdr, GcHeapKind::Object);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"a\":3,\"b\":2}");
}

TEST(JsValueEncodeTest, EncodeArrayWithStrings) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 3);
    ASSERT_NE(arr, nullptr);

    JsValue str = JsValue::make_string(heap, "hi", 2);
    ASSERT_EQ(js_value_type(str), JsNodeType::String);

    arr->elems[0] = JsValue::make_integer(1);
    arr->elems[1] = JsValue::make_boolean(false);
    arr->elems[2] = str;
    arr->size = 3;

    JsValue root = js_make_heap_ref(&arr->hdr, GcHeapKind::Array);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "[1,false,\"hi\"]");
}

TEST(JsValueEncodeTest, EncodeUtf16HeapStringWithoutTemporaryUtf8String) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);

    const char16_t text[] = {static_cast<char16_t>(0x4E2D)};
    GcString *str = fiber::script::gc_new_string_utf16(&heap, text, 1);
    ASSERT_NE(str, nullptr);
    JsValue root = js_make_heap_ref(&str->hdr, GcHeapKind::String);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);

    std::string expected = "\"";
    expected.append("\xE4\xB8\xAD", 3);
    expected.push_back('"');
    EXPECT_EQ(sink.output, expected);
}

TEST(JsValueEncodeTest, RejectsMalformedUtf16HeapStringBeforeWriting) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);

    const char16_t bad[] = {static_cast<char16_t>(0xD800)};
    GcString *str = fiber::script::gc_new_string_utf16(&heap, bad, 1);
    ASSERT_NE(str, nullptr);
    JsValue root = js_make_heap_ref(&str->hdr, GcHeapKind::String);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::InvalidString);
    EXPECT_TRUE(sink.output.empty());
}

TEST(JsValueEncodeTest, EncodeExceptionObject) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *meta_obj = fiber::script::gc_new_object(&heap, 2);
    ASSERT_NE(meta_obj, nullptr);
    GcString *code_key = make_key(heap, "code");
    ASSERT_NE(code_key, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, meta_obj, code_key, JsValue::make_integer(7)));
    JsValue meta = js_make_heap_ref(&meta_obj->hdr, GcHeapKind::Object);

    const char *name = "TypeError";
    const char *message = "boom";
    GcException *exc =
            fiber::script::gc_new_exception(&heap, 42, name, std::strlen(name), message, std::strlen(message), meta);
    ASSERT_NE(exc, nullptr);

    JsValue root = js_make_heap_ref(&exc->hdr, GcHeapKind::Exception);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"position\":42,\"name\":\"TypeError\",\"message\":\"boom\",\"meta\":{\"code\":7}}");
}

TEST(JsValueEncodeTest, EncodeExceptionDefaultMeta) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const char *name = "RangeError";
    GcException *exc = fiber::script::gc_new_exception(&heap, -1, name, std::strlen(name), nullptr, 0);
    ASSERT_NE(exc, nullptr);

    JsValue root = js_make_heap_ref(&exc->hdr, GcHeapKind::Exception);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"position\":-1,\"name\":\"RangeError\",\"message\":null,\"meta\":null}");
}

TEST(JsValueEncodeTest, StepsMatchOneShotAtEveryLimit) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const std::string borrowed(20000, 'b');
    const std::vector<std::uint8_t> borrowed_bytes(20000, 7);
    const JsValue root = mixed_document(heap, borrowed, borrowed_bytes);

    const std::string whole = encode_whole(root);
    EXPECT_GT(whole.size(), 400000U);
    for (const std::size_t limit: {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{100}, std::size_t{4096},
                                   std::size_t{16384}, std::size_t{65536}, std::size_t{1} << 20}) {
        EXPECT_EQ(encode_in_steps(root, limit), whole) << limit;
    }
}

TEST(JsValueEncodeTest, SlicedValuesMatchAcrossLengths) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    // Around the slice sizes, where off-by-one errors would show as lost or repeated bytes.
    for (const std::size_t len:
         {std::size_t{12287}, std::size_t{12288}, std::size_t{12289}, std::size_t{16383}, std::size_t{16384},
          std::size_t{16385}, std::size_t{24576}, std::size_t{24577}, std::size_t{32768}, std::size_t{32769}}) {
        const std::string text(len, 'x');
        EXPECT_EQ(encode_in_steps(heap_string(heap, text), 1), "\"" + text + "\"") << len;

        const std::vector<std::uint8_t> bytes(len, 0xAB);
        const JsValue binary = JsValue::make_binary(heap, bytes.data(), bytes.size());
        StringSink whole_sink;
        Generator whole(whole_sink);
        ASSERT_EQ(whole.binary(bytes.data(), bytes.size()), Generator::Result::OK);
        EXPECT_EQ(encode_in_steps(binary, 1), whole_sink.output) << len;
    }
}

TEST(JsValueEncodeTest, RunNeverStopsEarlyWhenTheValueEndsAtTheLimit) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const JsValue root = array_of(heap, {JsValue::make_integer(1)});

    StringSink sink;
    Generator gen(sink);
    JsValueEncoder encoder(gen, root);
    EXPECT_EQ(encoder.run(3), JsValueEncoder::Status::Done);
    EXPECT_EQ(sink.output, "[1]");
    EXPECT_EQ(encoder.run(3), JsValueEncoder::Status::Done);
    EXPECT_EQ(sink.output, "[1]");
}

TEST(JsValueEncodeTest, ZeroLimitStillMakesProgress) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const JsValue root = array_of(heap, {JsValue::make_integer(1), JsValue::make_integer(2)});
    EXPECT_EQ(encode_in_steps(root, 0), "[1,2]");
}

TEST(JsValueEncodeTest, LongStringsSliceOnCodePointBoundaries) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    // The prefix moves every multi-byte sequence across the slice cut. ValidateUtf8 checks each
    // slice alone, so a sequence split by a cut would fail the encode.
    for (const std::string_view unit:
         {std::string_view("\xC3\xA9"), std::string_view("\xE4\xB8\xAD"), std::string_view("\xF0\x9F\x98\x80")}) {
        for (std::size_t prefix = 0; prefix < 4; ++prefix) {
            std::string text(prefix, 'a');
            while (text.size() < 70000) {
                text.append(unit);
            }
            const JsValue value = heap_string(heap, text);

            StringSink sink;
            Generator gen(sink);
            gen.set_option(Generator::Option::ValidateUtf8, true);
            JsValueEncoder encoder(gen, value);
            EXPECT_EQ(encoder.run(std::numeric_limits<std::size_t>::max()), JsValueEncoder::Status::Done);
            EXPECT_EQ(sink.output, "\"" + text + "\"");
        }
    }
}

TEST(JsValueEncodeTest, InvalidUtf8BorrowedStringStillSlices) {
    // Nothing but continuation bytes: there is no boundary to back up to.
    const std::string text(40000, '\x80');
    const JsValue value = JsValue::make_native_string(text.data(), text.size());
    EXPECT_EQ(encode_in_steps(value, 1), "\"" + text + "\"");
}

TEST(JsValueEncodeTest, EscapedStringStaysNearTheLimit) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    // Sixfold growth is the worst case. encode_in_steps checks the overshoot.
    const std::string text(1 << 20, '\x01');
    const std::string encoded = encode_in_steps(heap_string(heap, text), std::size_t{64} * 1024);
    EXPECT_EQ(encoded.size(), (text.size() * 6) + 2);
}

TEST(JsValueEncodeTest, FlatArrayStopsAtTheLimit) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    // No child containers to step into, so only the limit can end a run. encode_in_steps checks that.
    const std::vector<JsValue> values(300000, JsValue::make_integer(1));
    const JsValue root = array_of(heap, values);
    EXPECT_EQ(encode_in_steps(root, 4096), encode_whole(root));
}

TEST(JsValueEncodeTest, FlatObjectStopsAtTheLimit) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcObject *obj = fiber::script::gc_new_object(&heap, 30000);
    ASSERT_NE(obj, nullptr);
    for (int i = 0; i < 30000; ++i) {
        const std::string key = "key" + std::to_string(i);
        GcString *name = fiber::script::gc_new_string(&heap, key.data(), key.size());
        ASSERT_NE(name, nullptr);
        ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, name, JsValue::make_integer(i)));
    }
    const JsValue root = js_make_heap_ref(&obj->hdr, GcHeapKind::Object);
    EXPECT_EQ(encode_in_steps(root, 4096), encode_whole(root));
}

TEST(JsValueEncodeTest, NonFiniteNumbersEncodeAsStrings) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    EXPECT_EQ(encode_whole(JsValue::make_float(nan)), "\"NaN\"");
    EXPECT_EQ(encode_whole(JsValue::make_float(-nan)), "\"NaN\"");
    EXPECT_EQ(encode_whole(JsValue::make_float(inf)), "\"Infinity\"");
    EXPECT_EQ(encode_whole(JsValue::make_float(-inf)), "\"-Infinity\"");

    GcObject *obj = fiber::script::gc_new_object(&heap, 1);
    ASSERT_NE(obj, nullptr);
    ASSERT_TRUE(fiber::script::gc_object_set(&heap, obj, make_key(heap, "a"), JsValue::make_float(nan)));
    const JsValue root = array_of(heap, {js_make_heap_ref(&obj->hdr, GcHeapKind::Object), JsValue::make_float(inf),
                                         JsValue::make_float(0.5)});
    EXPECT_EQ(encode_whole(root), "[{\"a\":\"NaN\"},\"Infinity\",0.5]");
}

TEST(JsValueEncodeTest, FailureAfterYieldStaysFailed) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    const char16_t bad[] = {static_cast<char16_t>(0xD800)};
    GcString *lone_surrogate = fiber::script::gc_new_string_utf16(&heap, bad, 1);
    ASSERT_NE(lone_surrogate, nullptr);
    const JsValue root =
            array_of(heap, {heap_string(heap, "first"), JsValue::make_integer(1),
                            js_make_heap_ref(&lone_surrogate->hdr, GcHeapKind::String), JsValue::make_integer(2)});

    StringSink sink;
    Generator gen(sink);
    JsValueEncoder encoder(gen, root);
    EXPECT_EQ(encoder.run(1), JsValueEncoder::Status::Yield);
    EXPECT_EQ(encoder.run(std::numeric_limits<std::size_t>::max()), JsValueEncoder::Status::Failed);
    EXPECT_EQ(encoder.error(), Generator::Result::InvalidString);
    EXPECT_EQ(sink.output, "[\"first\",1");

    EXPECT_EQ(encoder.run(1), JsValueEncoder::Status::Failed);
    EXPECT_EQ(encoder.error(), Generator::Result::InvalidString);
    EXPECT_EQ(sink.output, "[\"first\",1");
}

TEST(JsValueEncodeTest, SinkFailureFailsTheRun) {
    class RefusingSink final : public OutputSink {
    public:
        [[nodiscard]] bool write(const char * /*data*/, size_t /*len*/) noexcept override { return false; }
    };

    RefusingSink sink;
    Generator gen(sink);
    JsValueEncoder encoder(gen, JsValue::make_integer(1));
    EXPECT_EQ(encoder.run(1), JsValueEncoder::Status::Failed);
    EXPECT_EQ(encoder.error(), Generator::Result::ErrorState);
}

TEST(JsValueEncodeTest, DepthLimitAppliesToNestedValues) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    JsValue allowed = JsValue::make_integer(1);
    for (std::size_t i = 0; i + 1 < Generator::MAX_STACK; ++i) {
        allowed = array_of(heap, {allowed});
    }
    const std::string encoded = encode_whole(allowed);
    EXPECT_EQ(encoded.size(), ((Generator::MAX_STACK - 1) * 2) + 1);

    const JsValue too_deep = array_of(heap, {allowed});
    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, too_deep), Generator::Result::MaxDepthExceeded);
}

TEST(JsValueEncodeTest, CycleEndsAtTheDepthLimit) {
    GcHeap heap;
    GcHeap::NoGcScope no_gc(heap);
    GcArray *arr = fiber::script::gc_new_array(&heap, 1);
    ASSERT_NE(arr, nullptr);
    const JsValue root = js_make_heap_ref(&arr->hdr, GcHeapKind::Array);
    arr->elems[0] = root;
    arr->size = 1;

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, root), Generator::Result::MaxDepthExceeded);
}
