#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "script/JsGc.h"
#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::AbiResult;
using fiber::script::ScriptResult;
using fiber::script::ConstValueHandle;
using fiber::script::ExceptionKind;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::std_lib::StdLibrary;

namespace {

const GcString *as_string(const JsValue &value) { return js_value_heap_ptr<const GcString>(value); }

std::string string_to_utf8(const JsValue &value) {
    std::string out;
    if (fiber::script::js_value_is_borrowed_string(value)) {
        fiber::script::NativeStr native = fiber::script::js_value_native_string(value);
        out.assign(native.data, native.len);
        return out;
    }
    auto *str = as_string(value);
    if (str) {
        EXPECT_TRUE(fiber::script::gc_string_to_utf8(str, out));
    }
    return out;
}

ScriptResult run_script(std::string_view source, GcHeap &heap) {
    auto compiled = fiber::script::compile_script(StdLibrary::instance(), source);
    EXPECT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().message);
    if (!compiled) {
        return ScriptResult::abort(ScriptAbortReason::Internal);
    }
    JsValue root = JsValue::make_undefined();
    return compiled->exec_sync(root, nullptr, heap);
}

void expect_script_string(std::string_view source, std::string_view expected) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

void expect_caught(std::string_view source) {
    GcHeap heap;
    auto result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), "caught");
}

// Resolves a host function and invokes it directly with one JsValue argument, mirroring how
// the interpreter dispatches host calls. Used to assert undefined returns and exception kinds.
AbiResult call_host(const char *name, JsValue arg) {
    GcHeap heap;
    JsValue storage[1] = {arg};
    auto match = StdLibrary::instance().resolve_func(
            name, Library::FunctionMatchRequest{.known_argc = 1, .has_spread = false, .spread_argc_unknown = false});
    EXPECT_EQ(match.status, Library::FunctionMatchStatus::Found);
    if (match.status != Library::FunctionMatchStatus::Found || match.callable == nullptr ||
        match.callable->function == nullptr) {
        return AbiResult::abort(ScriptAbortReason::Internal);
    }
    Library::HostCallFrame frame(heap, JsValue::make_undefined(), nullptr);
    Library::Arguments args{ConstValueHandle(storage), 1};
    return match.callable->function(match.callable->userdata, frame, args);
}

void expect_undefined(const char *name, JsValue arg) {
    AbiResult result = call_host(name, arg);
    ASSERT_TRUE(result.is_success()) << name << " did not succeed";
    EXPECT_EQ(js_value_type(result.value()), JsNodeType::Undefined) << name;
}

void expect_exception_kind(const char *name, JsValue arg, ExceptionKind kind) {
    AbiResult result = call_host(name, arg);
    ASSERT_TRUE(result.is_exception()) << name << " did not raise";
    EXPECT_EQ(js_value_exception_kind(result.exception()), kind) << name;
}

} // namespace

// ---- binary.hex (Binary -> lowercase hex String) ----

TEST(BinaryFuncsTest, HexRoundTripsFromHex) {
    expect_script_string("return binary.hex(binary.fromHex(\"48656c6c6f\"));", "48656c6c6f");
}
TEST(BinaryFuncsTest, HexLowercasesUppercaseInput) {
    expect_script_string("return binary.hex(binary.fromHex(\"DEADBEEF\"));", "deadbeef");
}
TEST(BinaryFuncsTest, HexOfEmptyBinaryIsEmpty) { expect_script_string("return binary.hex(binary.fromHex(\"\"));", ""); }
TEST(BinaryFuncsTest, HexOfGetUtf8Bytes) {
    expect_script_string("return binary.hex(binary.getUtf8Bytes(\"abc\"));", "616263");
}
TEST(BinaryFuncsTest, HexNonBinaryIsCatchable) {
    expect_caught("try { binary.hex(123); return \"no\"; } catch (e) { return \"caught\"; }");
}

// ---- binary.fromHex (String -> Binary, strict) ----

TEST(BinaryFuncsTest, FromHexRoundTrips) {
    expect_script_string("return binary.hex(binary.fromHex(\"deadbeef\"));", "deadbeef");
}
TEST(BinaryFuncsTest, FromHexEmptyIsEmpty) { expect_script_string("return binary.hex(binary.fromHex(\"\"));", ""); }
TEST(BinaryFuncsTest, FromHexOddLengthRaises) {
    expect_caught("try { binary.fromHex(\"abc\"); return \"no\"; } catch (e) { return \"caught\"; }");
}
TEST(BinaryFuncsTest, FromHexNonHexCharRaises) {
    expect_caught("try { binary.fromHex(\"xy\"); return \"no\"; } catch (e) { return \"caught\"; }");
}
TEST(BinaryFuncsTest, FromHexNonStringIsCatchable) {
    expect_caught("try { binary.fromHex(123); return \"no\"; } catch (e) { return \"caught\"; }");
}

// ---- binary.base64Encode (Binary -> base64 String) ----

TEST(BinaryFuncsTest, Base64EncodeKnownVector) {
    // "Hello" -> SGVsbG8=
    expect_script_string("return binary.base64Encode(binary.fromHex(\"48656c6c6f\"));", "SGVsbG8=");
}
TEST(BinaryFuncsTest, Base64EncodeHighBytes) {
    // [0x00, 0xff] -> AP8=
    expect_script_string("return binary.base64Encode(binary.fromHex(\"00ff\"));", "AP8=");
}
TEST(BinaryFuncsTest, Base64EncodeEmptyIsEmpty) {
    expect_script_string("return binary.base64Encode(binary.fromHex(\"\"));", "");
}
TEST(BinaryFuncsTest, Base64EncodeNonBinaryIsUndefined) {
    expect_undefined("binary.base64Encode", JsValue::make_integer(123));
}

// ---- binary.base64Decode (String -> Binary, strict) ----

TEST(BinaryFuncsTest, Base64DecodeRoundTrip) {
    expect_script_string("return binary.hex(binary.base64Decode(\"SGVsbG8=\"));", "48656c6c6f");
}
TEST(BinaryFuncsTest, Base64DecodeThenEncodeRoundTrip) {
    expect_script_string("return binary.base64Encode(binary.base64Decode(\"SGVsbG8=\"));", "SGVsbG8=");
}
TEST(BinaryFuncsTest, Base64DecodeOneByte) {
    // AQ== -> [0x01]
    expect_script_string("return binary.hex(binary.base64Decode(\"AQ==\"));", "01");
}
TEST(BinaryFuncsTest, Base64DecodeHighBytes) {
    expect_script_string("return binary.hex(binary.base64Decode(\"AP8=\"));", "00ff");
}
TEST(BinaryFuncsTest, Base64DecodeEmptyIsEmpty) {
    expect_script_string("return binary.hex(binary.base64Decode(\"\"));", "");
}
TEST(BinaryFuncsTest, Base64DecodeNonStringIsUndefined) {
    expect_undefined("binary.base64Decode", JsValue::make_integer(123));
}
TEST(BinaryFuncsTest, Base64DecodeInvalidRaises) {
    // length not a multiple of 4
    expect_caught("try { binary.base64Decode(\"A\"); return \"no\"; } catch (e) { return \"caught\"; }");
    // overlong padding
    expect_caught("try { binary.base64Decode(\"A===\"); return \"no\"; } catch (e) { return \"caught\"; }");
    // '=' in the middle
    expect_caught("try { binary.base64Decode(\"AB=C\"); return \"no\"; } catch (e) { return \"caught\"; }");
    // non-alphabet characters
    expect_caught("try { binary.base64Decode(\"####\"); return \"no\"; } catch (e) { return \"caught\"; }");
    // whitespace is not tolerated
    expect_caught("try { binary.base64Decode(\"SGVs bG8=\"); return \"no\"; } catch (e) { return \"caught\"; }");
}

// ---- binary.getUtf8Bytes (any -> Binary = JsonUtil.toString(value)) ----

TEST(BinaryFuncsTest, GetUtf8BytesString) {
    expect_script_string("return binary.hex(binary.getUtf8Bytes(\"abc\"));", "616263");
}
TEST(BinaryFuncsTest, GetUtf8BytesInteger) {
    expect_script_string("return binary.hex(binary.getUtf8Bytes(42));", "3432");
}
TEST(BinaryFuncsTest, GetUtf8BytesNull) {
    expect_script_string("return binary.hex(binary.getUtf8Bytes(null));", "6e756c6c");
}
TEST(BinaryFuncsTest, GetUtf8BytesTrue) {
    expect_script_string("return binary.hex(binary.getUtf8Bytes(true));", "74727565");
}
TEST(BinaryFuncsTest, GetUtf8BytesFalse) {
    expect_script_string("return binary.hex(binary.getUtf8Bytes(false));", "66616c7365");
}
TEST(BinaryFuncsTest, GetUtf8BytesObject) {
    // <ObjectNode>
    expect_script_string("return binary.hex(binary.getUtf8Bytes({}));", "3c4f626a6563744e6f64653e");
}
TEST(BinaryFuncsTest, GetUtf8BytesArray) {
    // <ArrayNode>
    expect_script_string("return binary.hex(binary.getUtf8Bytes([]));", "3c41727261794e6f64653e");
}
TEST(BinaryFuncsTest, GetUtf8BytesMultibyteUtf8) {
    // U+00E9 -> C3 A9
    expect_script_string("return binary.hex(binary.getUtf8Bytes(\"é\"));", "c3a9");
}

// ---- exception kinds (direct host calls) ----

TEST(BinaryFuncsTest, Base64DecodeInvalidRaisesRangeError) {
    expect_exception_kind("binary.base64Decode", JsValue::make_native_string("####", 4), ExceptionKind::RangeError);
    expect_exception_kind("binary.base64Decode", JsValue::make_native_string("A", 1), ExceptionKind::RangeError);
}
TEST(BinaryFuncsTest, FromHexInvalidRaisesRangeError) {
    expect_exception_kind("binary.fromHex", JsValue::make_native_string("abc", 3), ExceptionKind::RangeError);
    expect_exception_kind("binary.fromHex", JsValue::make_native_string("xy", 2), ExceptionKind::RangeError);
}
TEST(BinaryFuncsTest, HexNonBinaryRaisesTypeError) {
    expect_exception_kind("binary.hex", JsValue::make_integer(123), ExceptionKind::TypeError);
}
TEST(BinaryFuncsTest, FromHexNonStringRaisesTypeError) {
    expect_exception_kind("binary.fromHex", JsValue::make_integer(123), ExceptionKind::TypeError);
}
