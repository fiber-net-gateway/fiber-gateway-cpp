#include "LengthFunc.h"

#include "StdLibrary.h"

#include "../../common/json/Utf.h"
#include "../JsValue.h"
#include "../Library.h"
#include "../gc/GcInternal.h"

#include <cstdint>

namespace fiber::script::std_lib {

namespace {

// Java's String.length() is the UTF-16 code-unit count (a JS-like length).
// Runtime strings are stored as UTF-8 (borrowed/native strings and
// Byte-encoded GcStrings) or as UTF-16 (Utf16-encoded GcStrings). utf8_scan
// yields the UTF-16 unit count of a UTF-8 sequence in one pass (BMP codepoint
// -> 1, supplementary -> 2). Malformed UTF-8 -- only reachable from
// host-supplied byte strings, never JSON or literals -- falls back to the byte
// count rather than aborting the call.
std::int64_t string_utf16_length(const JsValue &value) noexcept {
    if (js_value_is_borrowed_string(value)) {
        NativeStr native = js_value_native_string(value);
        fiber::json::Utf8ScanResult scan;
        if (fiber::json::utf8_scan(native.data, native.len, scan)) {
            return static_cast<std::int64_t>(scan.utf16_len);
        }
        return static_cast<std::int64_t>(native.len);
    }
    const GcString *str = js_value_heap_ptr<const GcString>(value);
    if (str == nullptr) {
        return 0;
    }
    if (str->encoding == GcStringEncoding::Utf16) {
        return static_cast<std::int64_t>(str->len);
    }
    fiber::json::Utf8ScanResult scan;
    if (fiber::json::utf8_scan(reinterpret_cast<const char *>(str->data8), str->len, scan)) {
        return static_cast<std::int64_t>(scan.utf16_len);
    }
    return static_cast<std::int64_t>(str->len);
}

std::int64_t binary_length(const JsValue &value) noexcept {
    if (js_value_is_borrowed_binary(value)) {
        NativeBin bin = js_value_native_binary(value);
        return static_cast<std::int64_t>(bin.len);
    }
    const GcBinary *bin = js_value_heap_ptr<const GcBinary>(value);
    if (bin == nullptr) {
        return 0;
    }
    return static_cast<std::int64_t>(bin->len);
}

ScriptResult length_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                       Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // The single parameter defaults to null; length(null) == 0.
        return ScriptResult::success(JsValue::make_integer(0));
    }
    const JsValue &value = args.args[0];
    switch (js_value_type(value)) {
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            return ScriptResult::success(JsValue::make_integer(0));
        case JsNodeType::String:
            return ScriptResult::success(JsValue::make_integer(string_utf16_length(value)));
        case JsNodeType::Binary:
            return ScriptResult::success(JsValue::make_integer(binary_length(value)));
        case JsNodeType::Array:
            if (const GcArray *arr = js_value_heap_ptr<const GcArray>(value)) {
                return ScriptResult::success(JsValue::make_integer(static_cast<std::int64_t>(arr->size)));
            }
            return ScriptResult::success(JsValue::make_integer(0));
        case JsNodeType::Object:
            if (const GcObject *obj = js_value_heap_ptr<const GcObject>(value)) {
                return ScriptResult::success(JsValue::make_integer(static_cast<std::int64_t>(obj->size)));
            }
            return ScriptResult::success(JsValue::make_integer(0));
        case JsNodeType::Boolean:
        case JsNodeType::Integer:
        case JsNodeType::Float:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
        default:
            // Jackson's JsonNode.size() is 0 for scalar value nodes; the
            // iterator/exception types have no Jackson analogue and also map to 0.
            return ScriptResult::success(JsValue::make_integer(0));
    }
}

} // namespace

void register_length_func(StdLibrary &lib) {
    lib.register_func("length", Library::FunctionSignature{.required_argc = 0, .fixed_argc = 1, .variadic = false},
                      {JsValue::make_null()}, &length_fn, nullptr, "length");
}

} // namespace fiber::script::std_lib
