#include "script/std/JsonFuncs.h"

#include <fiber/script/std/StdLibrary.h>

#include <fiber/script/std/NodeText.h>

#include <fiber/common/json/JsonDecode.h>
#include <fiber/common/json/JsonEncode.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueDecode.h>
#include <fiber/script/json/JsValueEncode.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace fiber::script::std_lib {

namespace {

// Writes generated JSON into a std::string buffer. Mirrors the StringSink used by
// http_script/ScriptExchangeCtx and the JsValueEncode tests.
class StringSink final : public fiber::json::OutputSink {
public:
    explicit StringSink(std::string &out) noexcept : out_(out) {}
    [[nodiscard]] bool write(const char *data, std::size_t len) noexcept override {
        out_.append(data, len);
        return true;
    }

private:
    std::string &out_;
};

// Materializes a view as a heap String result, mapping make_string's OOM (returns
// undefined) to a graceful abort. Mirrors StringsFuncs' make_string_result.
AbiResult make_string_result(GcHeap &heap, std::string_view sv) noexcept {
    if (sv.empty()) {
        return AbiResult::success(JsValue::make_native_string("", 0));
    }
    JsValue result = JsValue::make_string(heap, sv.data(), sv.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// ---- JSON.parse(text) ----
// text must be a String; any other type raises a catchable TypeError (mirrors
// Java's "parseJson not support <type>"). A malformed document raises a catchable
// SyntaxError carrying the decoder's message and byte offset (mirrors JS JSON.parse,
// which throws SyntaxError). OOM while building that exception aborts.

AbiResult parse_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    GcHeap &heap = frame.runtime;

    JsValue arg = (args.args && args.argc >= 1) ? args.args[0] : JsValue::make_undefined();
    std::string_view text;
    if (!string_utf8_view(arg, text)) {
        return AbiResult::exception(JsValue::make_exception(ExceptionKind::TypeError));
    }

    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    if (!out) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    *out = JsValue::make_undefined();

    // An empty string_view may carry a null data pointer; the decoder takes
    // (data, len) and a null pointer with len 0 is replaced by a valid empty C
    // string to avoid any chance of a dangling dereference.
    const char *data = text.data();
    if (data == nullptr) {
        data = "";
    }

    fiber::json::ParseError error{};
    fiber::json::DecodeStatus status = fiber::script::json::decode_js_value(heap, data, text.size(), out, &error);
    if (status == fiber::json::DecodeStatus::Complete || status == fiber::json::DecodeStatus::Ok) {
        return AbiResult::success(*out);
    }

    // Parse failure: build a catchable SyntaxError with the decoder's message/offset.
    ValueHandle exc = heap.local_value();
    if (!exc) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    const char *msg = error.message ? error.message : "invalid json";
    std::size_t msg_len = std::strlen(msg);
    if (!gc_make_exception(&heap, exc, static_cast<std::int64_t>(error.offset), "SyntaxError", 11, msg, msg_len)) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::exception(*exc);
}

// ---- JSON.stringify(value) ----
// Serializes value to a JSON text string via the Generator + encode_js_value bridge
// (mirrors Jackson's writeValueAsString). Top-level undefined -> returns undefined
// (matches JS); top-level NaN/+/-Inf -> "null" (matches JS; the Generator rejects
// non-finite doubles, which would otherwise surface as an encode failure). Any other
// encode failure (InvalidValue/InvalidString/MaxDepthExceeded) raises a catchable
// TypeError. OOM on the result string aborts.

AbiResult stringify_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    GcHeap &heap = frame.runtime;

    JsValue value = (args.args && args.argc >= 1) ? args.args[0] : JsValue::make_undefined();
    JsNodeType type = js_value_type(value);

    if (type == JsNodeType::Undefined) {
        return AbiResult::success(JsValue::make_undefined());
    }

    if (type == JsNodeType::Float) {
        double d = js_value_double(value);
        if (std::isnan(d) || std::isinf(d)) {
            // Matches JS JSON.stringify: NaN/Infinity serialize as "null".
            return make_string_result(heap, std::string_view("null", 4));
        }
    }

    std::string out;
    StringSink sink(out);
    fiber::json::Generator gen(sink);
    fiber::json::Generator::Result result = fiber::script::json::encode_js_value(gen, value);
    if (result != fiber::json::Generator::Result::OK) {
        return AbiResult::exception(JsValue::make_exception(ExceptionKind::TypeError));
    }
    return make_string_result(heap, std::string_view(out));
}

} // namespace

void register_json_funcs(StdLibrary &lib) {
    lib.register_func("JSON.parse", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &parse_fn, nullptr, "JSON.parse");
    lib.register_func("JSON.stringify",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &stringify_fn,
                      nullptr, "JSON.stringify");
}

} // namespace fiber::script::std_lib
