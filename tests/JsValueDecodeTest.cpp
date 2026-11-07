#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <fiber/common/json/JsonEncode.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueDecode.h>
#include <fiber/script/json/JsValueEncode.h>

using fiber::json::DecodeStatus;
using fiber::json::Generator;
using fiber::json::OutputSink;
using fiber::json::ParseError;
using fiber::script::GcArray;
using fiber::script::GcHeap;
using fiber::script::GcObject;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::ValueHandle;

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

ValueHandle decode_ok(GcHeap &heap, const char *text) {
    ValueHandle out = heap.global_value();
    EXPECT_NE(out, nullptr);
    if (!out) {
        return nullptr;
    }
    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, text, std::strlen(text), out, &error);
    EXPECT_EQ(status, DecodeStatus::Complete) << (error.message ? error.message : "");
    return out;
}

GcString *make_key(GcHeap &heap, const char *key) { return fiber::script::gc_new_string(&heap, key, std::strlen(key)); }

const JsValue *object_get(GcHeap &heap, const GcObject *obj, const char *key) {
    GcHeap::NoGcScope no_gc(heap);
    GcString *key_str = make_key(heap, key);
    EXPECT_NE(key_str, nullptr);
    if (!key_str) {
        return nullptr;
    }
    return fiber::script::gc_object_get(obj, key_str);
}

std::string to_utf8(const JsValue &value) {
    const auto *str = fiber::script::js_value_heap_ptr<const GcString>(value);
    EXPECT_NE(str, nullptr);
    std::string out;
    EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    return out;
}

} // namespace

TEST(JsValueDecodeTest, DecodeScalarValues) {
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "null");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Null);
    }
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "true");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Boolean);
        EXPECT_TRUE(fiber::script::js_value_bool(*out));
    }
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "-42");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Integer);
        EXPECT_EQ(fiber::script::js_value_int64(*out), -42);
    }
    {
        GcHeap heap;
        ValueHandle out = decode_ok(heap, "1.25");
        ASSERT_NE(out, nullptr);
        EXPECT_EQ(fiber::script::js_value_type(*out), JsNodeType::Float);
        EXPECT_DOUBLE_EQ(fiber::script::js_value_double(*out), 1.25);
    }
}

TEST(JsValueDecodeTest, DecodesEscapedUnicodeString) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, R"("line\n\u00E9 \uD83D\uDE00")");
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::String);

    std::string expected("line\n", 5);
    expected.push_back(static_cast<char>(0xC3));
    expected.push_back(static_cast<char>(0xA9));
    expected.push_back(' ');
    expected.push_back(static_cast<char>(0xF0));
    expected.push_back(static_cast<char>(0x9F));
    expected.push_back(static_cast<char>(0x98));
    expected.push_back(static_cast<char>(0x80));
    EXPECT_EQ(to_utf8(*out), expected);
}

TEST(JsValueDecodeTest, DecodesArrayAndObject) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, R"({"items":[1,false,{"name":"fiber"}],"empty":{}})");
    ASSERT_NE(out, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::Object);

    auto *obj = fiber::script::js_value_heap_ptr<GcObject>(*out);
    ASSERT_NE(obj, nullptr);

    const JsValue *items_value = object_get(heap, obj, "items");
    ASSERT_NE(items_value, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*items_value), JsNodeType::Array);
    const auto *items = fiber::script::js_value_heap_ptr<const GcArray>(*items_value);
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size, 3U);
    EXPECT_EQ(fiber::script::js_value_int64(items->elems[0]), 1);
    EXPECT_FALSE(fiber::script::js_value_bool(items->elems[1]));

    ASSERT_EQ(fiber::script::js_value_type(items->elems[2]), JsNodeType::Object);
    const auto *nested = fiber::script::js_value_heap_ptr<const GcObject>(items->elems[2]);
    ASSERT_NE(nested, nullptr);
    const JsValue *name = object_get(heap, nested, "name");
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(to_utf8(*name), "fiber");

    const JsValue *empty = object_get(heap, obj, "empty");
    ASSERT_NE(empty, nullptr);
    ASSERT_EQ(fiber::script::js_value_type(*empty), JsNodeType::Object);
    const auto *empty_obj = fiber::script::js_value_heap_ptr<const GcObject>(*empty);
    ASSERT_NE(empty_obj, nullptr);
    EXPECT_EQ(empty_obj->size, 0U);
}

TEST(JsValueDecodeTest, DuplicateKeysOverwriteInOriginalOrder) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, R"({"a":1,"b":2,"a":3})");
    ASSERT_NE(out, nullptr);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, *out), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"a\":3,\"b\":2}");
}

TEST(JsValueDecodeTest, RejectsInvalidInput) {
    GcHeap heap;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, "{\"a\":1} trailing",
                                                               std::strlen("{\"a\":1} trailing"), out, &error);
    EXPECT_EQ(status, DecodeStatus::Error);
    EXPECT_NE(error.message, nullptr);
    EXPECT_TRUE(fiber::script::js_value_is_undefined(*out));

    status = fiber::script::json::decode_js_value(heap, "9223372036854775808", std::strlen("9223372036854775808"), out,
                                                  &error);
    EXPECT_EQ(status, DecodeStatus::Error);
    EXPECT_NE(error.message, nullptr);
    EXPECT_TRUE(fiber::script::js_value_is_undefined(*out));
}

TEST(JsValueDecodeTest, RejectsTooDeepJson) {
    GcHeap heap;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    std::string json;
    for (int i = 0; i < 140; ++i) {
        json.push_back('[');
    }
    for (int i = 0; i < 140; ++i) {
        json.push_back(']');
    }

    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, json.data(), json.size(), out, &error);
    EXPECT_NE(status, DecodeStatus::Complete);
    EXPECT_NE(error.message, nullptr);
    EXPECT_TRUE(fiber::script::js_value_is_undefined(*out));
}

TEST(JsValueDecodeTest, SurvivesCollectionDuringBuild) {
    GcHeap heap;
    heap.threshold = 1;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    const char *json = R"({"items":[{"name":"a","value":1},{"name":"b","value":2}],"ok":true})";
    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, json, std::strlen(json), out, &error);
    ASSERT_EQ(status, DecodeStatus::Complete) << (error.message ? error.message : "");

    heap.collect();

    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::Object);
    auto *obj = fiber::script::js_value_heap_ptr<GcObject>(*out);
    ASSERT_NE(obj, nullptr);
    const JsValue *items_value = object_get(heap, obj, "items");
    ASSERT_NE(items_value, nullptr);
    const auto *items = fiber::script::js_value_heap_ptr<const GcArray>(*items_value);
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size, 2U);

    const JsValue *ok = object_get(heap, obj, "ok");
    ASSERT_NE(ok, nullptr);
    EXPECT_TRUE(fiber::script::js_value_bool(*ok));
}

TEST(JsValueDecodeTest, DoesNotCollectDuringDecodeWhenThresholdCrossed) {
    GcHeap heap;
    // threshold=1 forces every decoded allocation to request a collection.
    heap.threshold = 1;
    ValueHandle out = heap.global_value();
    ASSERT_NE(out, nullptr);

    const char *json = R"({"items":[1,2,3],"name":"hello"})";
    ParseError error;
    DecodeStatus status = fiber::script::json::decode_js_value(heap, json, std::strlen(json), out, &error);
    ASSERT_EQ(status, DecodeStatus::Complete) << (error.message ? error.message : "");

    // Decoding crossed the limit repeatedly, but the deferred NoGcScope wrapped
    // around decode suppressed every collection (without it, the per-primitive
    // NoGcScopes inside gc_make_*/gc_array_push/gc_object_set would each fire a
    // full collect as depth returned to 0). A collect raises threshold to
    // >= 1MiB; observing it still at 1 proves none ran during decode.
    EXPECT_EQ(heap.threshold, 1U);

    // The decoded result stays rooted and intact through a post-decode collect.
    heap.collect();
    ASSERT_EQ(fiber::script::js_value_type(*out), JsNodeType::Object);
}

TEST(JsValueDecodeTest, RoundTripsThroughEncoder) {
    GcHeap heap;
    ValueHandle out = decode_ok(heap, R"({"n":1,"arr":[true,null,"x"]})");
    ASSERT_NE(out, nullptr);

    StringSink sink;
    Generator gen(sink);
    EXPECT_EQ(fiber::script::json::encode_js_value(gen, *out), Generator::Result::OK);
    EXPECT_EQ(sink.output, "{\"n\":1,\"arr\":[true,null,\"x\"]}");
}

// ---- JsValueStreamDecoder ----

namespace {

using fiber::script::json::JsValueStreamDecoder;

constexpr std::string_view kStreamDocument =
        "{\"text\":\"plain\",\"esc\":\"a\\n\\u00e9\\ud83d\\ude00\",\"utf8\":\"\xC3\xA9\xE4\xBD\xA0\","
        "\"numbers\":[0,-12,1.5,2.5e-3],\"nested\":{\"list\":[[],{},[1,[2,[3]]]],"
        "\"flags\":[true,false,null]}}";

std::string to_json(const JsValue &value) {
    std::string out;
    fiber::json::StringSink sink(out);
    fiber::json::Generator gen(sink);
    EXPECT_NE(fiber::script::json::encode_js_value(gen, value), fiber::json::Generator::Result::ErrorState);
    return out;
}

std::string whole_decode(std::string_view text) {
    GcHeap heap;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    EXPECT_EQ(fiber::script::json::decode_js_value(heap, text.data(), text.size(), out), DecodeStatus::Complete);
    return to_json(*out);
}

} // namespace

TEST(JsValueStreamDecoderTest, MatchesAWholeDecodeAtEverySplit) {
    const std::string expected = whole_decode(kStreamDocument);
    for (std::size_t split = 0; split <= kStreamDocument.size(); ++split) {
        GcHeap heap;
        GcHeap::LocalMark mark(heap);
        ValueHandle out = heap.local_value();
        JsValueStreamDecoder decoder(heap, out);
        const std::string_view head = kStreamDocument.substr(0, split);
        const std::string_view tail = kStreamDocument.substr(split);
        (void) decoder.feed(head.data(), head.size());
        (void) decoder.feed(tail.data(), tail.size());
        ASSERT_EQ(decoder.finish(), DecodeStatus::Complete) << split;
        EXPECT_EQ(to_json(*out), expected) << split;
    }
}

TEST(JsValueStreamDecoderTest, KeepsWhatItBuiltAcrossCollections) {
    const std::string expected = whole_decode(kStreamDocument);
    GcHeap heap;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    JsValueStreamDecoder decoder(heap, out);
    for (const char &byte: kStreamDocument) {
        ASSERT_NE(decoder.feed(&byte, 1), DecodeStatus::Error);
        heap.collect();
    }
    ASSERT_EQ(decoder.finish(), DecodeStatus::Complete);
    EXPECT_EQ(to_json(*out), expected);
}

TEST(JsValueStreamDecoderTest, FinishRejectsAnUnfinishedTokenAfterTheValue) {
    for (std::string_view text: {"1 2", "{} 3", "[] \"abc", "null tru"}) {
        GcHeap heap;
        GcHeap::LocalMark mark(heap);
        ValueHandle out = heap.local_value();
        JsValueStreamDecoder decoder(heap, out);
        (void) decoder.feed(text.data(), text.size());
        EXPECT_EQ(decoder.finish(), DecodeStatus::Error) << text;
        EXPECT_FALSE(decoder.error().out_of_memory) << text;
        EXPECT_EQ(js_value_type(*out), JsNodeType::Undefined) << text;
    }
}

TEST(JsValueStreamDecoderTest, ReportsATokenStillSpanningChunks) {
    GcHeap heap;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    JsValueStreamDecoder decoder(heap, out);
    EXPECT_EQ(decoder.pending_bytes(), 0U);
    (void) decoder.feed("[\"abcd", 6);
    EXPECT_EQ(decoder.pending_bytes(), 5U);
    (void) decoder.feed("efgh", 4);
    EXPECT_EQ(decoder.pending_bytes(), 9U);
    (void) decoder.feed("\"]", 2);
    EXPECT_EQ(decoder.pending_bytes(), 0U);
    EXPECT_EQ(decoder.finish(), DecodeStatus::Complete);
}

// Every limit below the document's peak must end in a clean refusal, reported as memory rather
// than as bad JSON, leaving nothing live behind.
TEST(JsValueStreamDecoderTest, EveryLimitFailsCleanly) {
    std::size_t peak = 0;
    {
        GcHeap heap;
        GcHeap::LocalMark mark(heap);
        ValueHandle out = heap.local_value();
        ASSERT_EQ(fiber::script::json::decode_js_value(heap, kStreamDocument.data(), kStreamDocument.size(), out),
                  DecodeStatus::Complete);
        peak = heap.stats().peak_bytes;
    }
    std::size_t refused = 0;
    for (std::size_t limit = 1; limit < peak; limit += 7) {
        GcHeap heap;
        heap.set_max_bytes(limit);
        {
            GcHeap::LocalMark mark(heap);
            ValueHandle out = heap.local_value();
            JsValueStreamDecoder decoder(heap, out);
            (void) decoder.feed(kStreamDocument.data(), kStreamDocument.size());
            const DecodeStatus status = decoder.finish();
            if (status != DecodeStatus::Complete) {
                ++refused;
                EXPECT_TRUE(decoder.error().out_of_memory) << limit;
                EXPECT_EQ(js_value_type(*out), JsNodeType::Undefined) << limit;
            }
        }
        heap.collect();
        heap.collect();
        EXPECT_EQ(heap.stats().current_bytes, 0U) << limit;
    }
    EXPECT_GT(refused, 0U);
}

TEST(JsValueDecodeTest, ReportsARefusedAllocationAsOutOfMemory) {
    GcHeap heap;
    heap.set_max_bytes(64);
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    ParseError error;
    EXPECT_NE(fiber::script::json::decode_js_value(heap, kStreamDocument.data(), kStreamDocument.size(), out, &error),
              DecodeStatus::Complete);
    EXPECT_TRUE(error.out_of_memory);
}

TEST(JsValueDecodeTest, RejectsAnUnfinishedTokenAfterTheValue) {
    for (std::string_view text: {"1 2", "{} 3", "[] \"abc", "null tru", "true -", "1 1.5"}) {
        GcHeap heap;
        GcHeap::LocalMark mark(heap);
        ValueHandle out = heap.local_value();
        ParseError error;
        EXPECT_EQ(fiber::script::json::decode_js_value(heap, text.data(), text.size(), out, &error),
                  DecodeStatus::Error)
                << text;
        EXPECT_FALSE(error.out_of_memory) << text;
        EXPECT_EQ(js_value_type(*out), JsNodeType::Undefined) << text;
    }
}

TEST(JsValueDecodeTest, AValueEndingAtTheEndOfInputIsComplete) {
    for (std::string_view text: {"12", "-1.5e3", "true", "null", "\"s\"", "{}", "[]", "7 ", "{}\n", " [] \t"}) {
        GcHeap heap;
        GcHeap::LocalMark mark(heap);
        ValueHandle out = heap.local_value();
        EXPECT_EQ(fiber::script::json::decode_js_value(heap, text.data(), text.size(), out), DecodeStatus::Complete)
                << text;
    }
}

TEST(JsValueDecodeTest, BadJsonIsNotOutOfMemory) {
    GcHeap heap;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    ParseError error;
    EXPECT_EQ(fiber::script::json::decode_js_value(heap, "{bad", 4, out, &error), DecodeStatus::Error);
    EXPECT_FALSE(error.out_of_memory);
}
