#include "script/std/LengthFunc.h"

#include <fiber/script/std/StdLibrary.h>

#include <fiber/common/json/Utf.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>

#include <cstdint>

namespace fiber::script::std_lib {

namespace {

// Java's String.length() is the UTF-16 code-unit count (a JS-like length).
// Heap strings cache this count beside canonical WTF-8 bytes. Borrowed strings
// are scanned on demand; malformed host input falls back to its byte count.
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
    return static_cast<std::int64_t>(str->utf16_len);
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

AbiResult length_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // The single parameter defaults to null; length(null) == 0.
        return AbiResult::success(JsValue::make_integer(0));
    }
    const JsValue &value = args.args[0];
    switch (js_value_type(value)) {
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            return AbiResult::success(JsValue::make_integer(0));
        case JsNodeType::String:
            return AbiResult::success(JsValue::make_integer(string_utf16_length(value)));
        case JsNodeType::Binary:
            return AbiResult::success(JsValue::make_integer(binary_length(value)));
        case JsNodeType::Array:
            if (const GcArray *arr = js_value_heap_ptr<const GcArray>(value)) {
                return AbiResult::success(JsValue::make_integer(static_cast<std::int64_t>(arr->size)));
            }
            return AbiResult::success(JsValue::make_integer(0));
        case JsNodeType::Object:
            if (const GcObject *obj = js_value_heap_ptr<const GcObject>(value)) {
                return AbiResult::success(JsValue::make_integer(static_cast<std::int64_t>(obj->size)));
            }
            return AbiResult::success(JsValue::make_integer(0));
        case JsNodeType::Boolean:
        case JsNodeType::Integer:
        case JsNodeType::Float:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
        default:
            // Jackson's JsonNode.size() is 0 for scalar value nodes; the
            // iterator/exception types have no Jackson analogue and also map to 0.
            return AbiResult::success(JsValue::make_integer(0));
    }
}

} // namespace

void register_length_func(StdLibrary &lib) {
    lib.register_func("length", Library::FunctionSignature{.required_argc = 0, .fixed_argc = 1, .variadic = false},
                      {JsValue::make_null()}, &length_fn, nullptr, "length");
}

} // namespace fiber::script::std_lib
