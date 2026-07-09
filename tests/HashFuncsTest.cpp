#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "script/JsGc.h"
#include "script/ScriptCompiler.h"
#include "script/gc/GcInternal.h"
#include "script/std/StdLibrary.h"

using fiber::script::ConstValueHandle;
using fiber::script::GcHeap;
using fiber::script::GcString;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ScriptResult;
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

void expect_script_int(std::string_view source, std::int64_t expected) {
    GcHeap heap;
    ScriptResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::Integer);
    EXPECT_EQ(fiber::script::js_value_int64(result.value()), expected);
}

void expect_script_string(std::string_view source, std::string_view expected) {
    GcHeap heap;
    ScriptResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

void expect_caught(std::string_view source, std::string_view expected) {
    GcHeap heap;
    ScriptResult result = run_script(source, heap);
    ASSERT_TRUE(result.is_success()) << "script did not succeed";
    ASSERT_EQ(js_value_type(result.value()), JsNodeType::String);
    EXPECT_EQ(string_to_utf8(result.value()), expected);
}

// Binary values can't be constructed in-script yet (BinaryFunc not migrated), so the
// binary branch is exercised by resolving the host function and invoking it directly with
// a borrowed-binary JsValue (mirrors how the interpreter dispatches host calls).
std::string call_hash_binary(const char *name, const std::uint8_t *bytes, std::size_t len) {
    GcHeap heap;
    JsValue args_storage[1] = {JsValue::make_native_binary(bytes, len)};
    auto match = StdLibrary::instance().resolve_func(
            name, Library::FunctionMatchRequest{.known_argc = 1, .has_spread = false, .spread_argc_unknown = false});
    EXPECT_EQ(match.status, Library::FunctionMatchStatus::Found);
    if (match.status != Library::FunctionMatchStatus::Found || match.callable == nullptr ||
        match.callable->function == nullptr) {
        return {};
    }
    Library::HostCallFrame frame(&heap, nullptr, nullptr);
    Library::Arguments args{ConstValueHandle(args_storage), 1};
    ScriptResult result = match.callable->function(match.callable->userdata, frame, args);
    EXPECT_TRUE(result.is_success()) << "hash did not succeed";
    if (!result.is_success()) {
        return {};
    }
    return string_to_utf8(result.value());
}

} // namespace

// ---- hash.crc32 (java.util.zip.CRC32 parity, asText(null) extraction) ----

TEST(HashFuncsTest, Crc32EmptyIsZero) { expect_script_int("return hash.crc32(\"\");", 0); }

TEST(HashFuncsTest, Crc32KnownVectors) {
    expect_script_int("return hash.crc32(\"a\");", 3904355907); // 0xE8B7BE43
    expect_script_int("return hash.crc32(\"abc\");", 891568578); // 0x352441C2
    expect_script_int("return hash.crc32(\"null\");", 634125391); // 0x25CBFC4F
}

TEST(HashFuncsTest, Crc32NullLiteralHashesNullText) {
    // null renders to "null" via asText, so crc32(null) == crc32("null").
    expect_script_int("return hash.crc32(null);", 634125391);
}

TEST(HashFuncsTest, Crc32CoercesScalarsViaAsText) {
    // Numbers/booleans hash their asText rendering.
    expect_script_int("return hash.crc32(123);", 2286445522); // crc32("123")
    expect_script_int("return hash.crc32(true);", 4261170317); // crc32("true")
    expect_script_int("return hash.crc32(false);", 734881840); // crc32("false")
}

TEST(HashFuncsTest, Crc32ContainersAndUndefinedCollapseToZero) {
    expect_script_int("return hash.crc32([]);", 0);
    expect_script_int("return hash.crc32({});", 0);
}

// ---- hash.md5 / sha1 / sha256 (lowercase hex, String branch) ----

TEST(HashFuncsTest, Md5StringBranch) {
    expect_script_string("return hash.md5(\"a\");", "0cc175b9c0f1b6a831c399e269772661");
    expect_script_string("return hash.md5(\"\");", "d41d8cd98f00b204e9800998ecf8427e");
    expect_script_string("return hash.md5(\"abc\");", "900150983cd24fb0d6963f7d28e17f72");
}

TEST(HashFuncsTest, Sha1StringBranch) {
    expect_script_string("return hash.sha1(\"a\");", "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8");
    expect_script_string("return hash.sha1(\"\");", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    expect_script_string("return hash.sha1(\"abc\");", "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(HashFuncsTest, Sha256StringBranch) {
    expect_script_string("return hash.sha256(\"a\");",
                         "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb");
    expect_script_string("return hash.sha256(\"\");",
                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    expect_script_string("return hash.sha256(\"abc\");",
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(HashFuncsTest, DigestHexLengths) {
    // length() counts UTF-16 units; ASCII hex == byte count.
    expect_script_int("return length(hash.md5(\"a\"));", 32);
    expect_script_int("return length(hash.sha1(\"a\"));", 40);
    expect_script_int("return length(hash.sha256(\"a\"));", 64);
}

// ---- hash.md5 / sha1 / sha256 (Binary branch, direct host call) ----

TEST(HashFuncsTest, Md5BinaryBranch) {
    static constexpr std::uint8_t kBytes[] = {0x00, 0x01, 0xFF};
    EXPECT_EQ(call_hash_binary("hash.md5", kBytes, 3), "ffbb8cd5a232b7d906904533e9609f48");
}

TEST(HashFuncsTest, Sha1BinaryBranch) {
    static constexpr std::uint8_t kBytes[] = {0x00, 0x01, 0xFF};
    EXPECT_EQ(call_hash_binary("hash.sha1", kBytes, 3), "c63e8274458bc7501e7c981f6394ced6d4490fda");
}

TEST(HashFuncsTest, Sha256BinaryBranch) {
    static constexpr std::uint8_t kBytes[] = {0x00, 0x01, 0xFF};
    EXPECT_EQ(call_hash_binary("hash.sha256", kBytes, 3),
              "26a66b061e8f48f39927c312f25293959729eee95978e2892d49d3512a5cc092");
}

// ---- unsupported input types raise catchable TypeError ----

TEST(HashFuncsTest, UnsupportedTypesRaiseTypeError) {
    expect_caught("try { hash.md5(123); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_caught("try { hash.md5(null); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_caught("try { hash.md5([]); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_caught("try { hash.sha1(true); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
    expect_caught("try { hash.sha256({}); return \"no-throw\"; } catch (e) { return \"caught\"; }", "caught");
}
