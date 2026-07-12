#include "IncludesFunc.h"

#include "StdLibrary.h"

#include "../JsValue.h"
#include "../Library.h"
#include "../gc/GcInternal.h"
#include "../run/Compares.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::script::std_lib {

namespace {

// Mirrors InterpreterVm's const_handle: ConstValueHandle exposes only const
// accessors yet is constructed from JsValue*, so a const_cast bridges the gap.
ConstValueHandle const_handle(const JsValue &value) noexcept { return ConstValueHandle(const_cast<JsValue *>(&value)); }

// Returns a borrowed UTF-8 view; heap strings with isolated UTF-16 surrogates
// are rejected. For well-formed Unicode a
// UTF-8 byte-substring search matches Java's UTF-16 String.contains, since
// containment of a codepoint sequence is encoding-independent.
bool string_utf8_view(const JsValue &value, std::string_view &out) noexcept {
    out = {};
    if (js_value_type(value) != JsNodeType::String) {
        return false;
    }
    if (js_value_is_borrowed_string(value)) {
        NativeStr native = js_value_native_string(value);
        if (native.len > 0 && !native.data) {
            return false;
        }
        out = native.len > 0 ? std::string_view(native.data, native.len) : std::string_view{};
        return true;
    }
    const GcString *str = js_value_heap_ptr<const GcString>(value);
    if (str == nullptr) {
        return false;
    }
    return gc_string_utf8_view(str, out);
}

AbiResult includes_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // No container argument: neither a text nor array container.
        return AbiResult::success(JsValue::make_boolean(false));
    }
    const JsValue &container = args.args[0];
    JsNodeType ctype = js_value_type(container);

    if (ctype == JsNodeType::String) {
        std::string_view text;
        if (!string_utf8_view(container, text)) {
            return AbiResult::success(JsValue::make_boolean(false));
        }
        for (std::uint32_t i = 1; i < args.argc; ++i) {
            std::string_view item;
            if (!string_utf8_view(args.args[i], item)) {
                // A non-text item cannot be a substring of a text container.
                return AbiResult::success(JsValue::make_boolean(false));
            }
            if (text.find(item) == std::string::npos) {
                return AbiResult::success(JsValue::make_boolean(false));
            }
        }
        return AbiResult::success(JsValue::make_boolean(true));
    }

    if (ctype == JsNodeType::Array) {
        const GcArray *arr = js_value_heap_ptr<const GcArray>(container);
        if (arr == nullptr) {
            return AbiResult::success(JsValue::make_boolean(false));
        }
        for (std::uint32_t i = 1; i < args.argc; ++i) {
            ConstValueHandle item = const_handle(args.args[i]);
            bool found = false;
            for (std::size_t j = 0; j < arr->size; ++j) {
                // Java's Compares.includes(array, item) uses strict (===) per
                // element; reuse the language's own strict equality.
                if (run::Compares::seq(const_handle(arr->elems[j]), item)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return AbiResult::success(JsValue::make_boolean(false));
            }
        }
        return AbiResult::success(JsValue::make_boolean(true));
    }

    // Container is neither string nor array.
    return AbiResult::success(JsValue::make_boolean(false));
}

} // namespace

void register_includes_func(StdLibrary &lib) {
    lib.register_func("includes", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = true},
                      &includes_fn, nullptr, "includes");
}

} // namespace fiber::script::std_lib
