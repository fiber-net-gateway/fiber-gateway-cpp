#include <fiber/script/std/ArrayFuncs.h>

#include <fiber/script/std/StdLibrary.h>

#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>

#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>

namespace fiber::script::std_lib {

namespace {

// Mirrors Jackson's JsonNode.asText("") used by the Java ArrayFuncs: scalars render to their
// textual form, while null/undefined/containers/binaries collapse to "" (the supplied default).
void append_as_text(const JsValue &value, std::string &out) {
    switch (js_value_type(value)) {
        case JsNodeType::String: {
            if (js_value_is_borrowed_string(value)) {
                NativeStr native = js_value_native_string(value);
                if (native.len > 0 && native.data != nullptr) {
                    out.append(native.data, native.len);
                }
                return;
            }
            const GcString *str = js_value_heap_ptr<const GcString>(value);
            if (str != nullptr) {
                std::string_view view;
                if (gc_string_utf8_view(str, view) && !view.empty()) {
                    out.append(view);
                }
            }
            return;
        }
        case JsNodeType::Integer: {
            char buffer[32];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), js_value_int64(value));
            if (converted.ec == std::errc{}) {
                out.append(buffer, static_cast<std::size_t>(converted.ptr - buffer));
            }
            return;
        }
        case JsNodeType::Float: {
            double number = js_value_double(value);
            if (std::isnan(number)) {
                out.append("NaN", 3);
                return;
            }
            if (std::isinf(number)) {
                out.append(number < 0 ? "-Infinity" : "Infinity", number < 0 ? 9 : 8);
                return;
            }
            char buffer[64];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number);
            if (converted.ec == std::errc{}) {
                out.append(buffer, static_cast<std::size_t>(converted.ptr - buffer));
            }
            return;
        }
        case JsNodeType::Boolean:
            out.append(js_value_bool(value) ? "true" : "false", js_value_bool(value) ? 4 : 5);
            return;
        case JsNodeType::Undefined:
        case JsNodeType::Null:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
        case JsNodeType::Binary:
        default:
            return;
    }
}

AbiResult type_error() noexcept { return AbiResult::exception(JsValue::make_exception(ExceptionKind::TypeError)); }

const GcArray *as_array(ConstValueHandle handle) noexcept {
    if (!handle) {
        return nullptr;
    }
    const JsValue &value = handle[0];
    if (js_value_type(value) != JsNodeType::Array) {
        return nullptr;
    }
    return js_value_heap_ptr<const GcArray>(value);
}

AbiResult array_join_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    const GcArray *arr = as_array(args.args);
    if (arr == nullptr) {
        return type_error();
    }

    std::string delimiter;
    if (args.argc >= 2) {
        append_as_text(args.args[1], delimiter);
    }

    std::string out;
    for (std::size_t i = 0; i < arr->size; ++i) {
        if (i > 0) {
            out.append(delimiter);
        }
        append_as_text(arr->elems[i], out);
    }
    JsValue result = JsValue::make_string(frame.runtime, out.data(), out.size());
    return AbiResult::success(result);
}

AbiResult array_pop_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                       Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    JsValue arr_val = args.args[0];
    if (js_value_type(arr_val) != JsNodeType::Array) {
        return type_error();
    }
    JsValue popped;
    if (!gc_array_pop(ValueHandle(&arr_val), ValueHandle(&popped))) {
        return AbiResult::success(JsValue::make_null());
    }
    return AbiResult::success(popped);
}

AbiResult array_push_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    JsValue arr_val = args.args[0];
    if (js_value_type(arr_val) != JsNodeType::Array) {
        return type_error();
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    for (std::uint32_t i = 1; i < args.argc; ++i) {
        if (!gc_array_push(heap, ValueHandle(&arr_val), args.args[i])) {
            return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
    }
    return AbiResult::success(arr_val);
}

} // namespace

void register_array_funcs(StdLibrary &lib) {
    lib.register_func("array.join", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 2, .variadic = false},
                      {JsValue::make_native_string("", 0)}, &array_join_fn, nullptr, "array.join");
    lib.register_func("array.pop", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &array_pop_fn, nullptr, "array.pop");
    lib.register_func("array.push", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = true},
                      &array_push_fn, nullptr, "array.push");
}

} // namespace fiber::script::std_lib
