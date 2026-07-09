#include "IncludesFunc.h"

#include "StdLibrary.h"

#include "../JsValue.h"
#include "../Library.h"
#include "../gc/GcInternal.h"
#include "../run/Compares.h"

#include <cstdint>
#include <string>

namespace fiber::script::std_lib {

namespace {

// Mirrors InterpreterVm's const_handle: ConstValueHandle exposes only const
// accessors yet is constructed from JsValue*, so a const_cast bridges the gap.
ConstValueHandle const_handle(const JsValue &value) noexcept { return ConstValueHandle(const_cast<JsValue *>(&value)); }

// Fills out with the UTF-8 bytes of a string value; returns false when value is
// not a string. Borrowed/native strings are already UTF-8; Byte/Utf16 GcStrings
// are normalized to UTF-8 via gc_string_to_utf8. For well-formed Unicode a
// UTF-8 byte-substring search matches Java's UTF-16 String.contains, since
// containment of a codepoint sequence is encoding-independent.
bool string_to_utf8(const JsValue &value, std::string &out) noexcept {
    if (js_value_type(value) != JsNodeType::String) {
        return false;
    }
    if (js_value_is_borrowed_string(value)) {
        NativeStr native = js_value_native_string(value);
        out.assign(native.data, native.len);
        return true;
    }
    const GcString *str = js_value_heap_ptr<const GcString>(value);
    if (str == nullptr) {
        return false;
    }
    return gc_string_to_utf8(str, out);
}

ScriptResult includes_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                         Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // No container argument: neither a text nor array container.
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    const JsValue &container = args.args[0];
    JsNodeType ctype = js_value_type(container);

    if (ctype == JsNodeType::String) {
        std::string text;
        if (!string_to_utf8(container, text)) {
            return ScriptResult::success(JsValue::make_boolean(false));
        }
        for (std::uint32_t i = 1; i < args.argc; ++i) {
            std::string item;
            if (!string_to_utf8(args.args[i], item)) {
                // A non-text item cannot be a substring of a text container.
                return ScriptResult::success(JsValue::make_boolean(false));
            }
            if (text.find(item) == std::string::npos) {
                return ScriptResult::success(JsValue::make_boolean(false));
            }
        }
        return ScriptResult::success(JsValue::make_boolean(true));
    }

    if (ctype == JsNodeType::Array) {
        const GcArray *arr = js_value_heap_ptr<const GcArray>(container);
        if (arr == nullptr) {
            return ScriptResult::success(JsValue::make_boolean(false));
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
                return ScriptResult::success(JsValue::make_boolean(false));
            }
        }
        return ScriptResult::success(JsValue::make_boolean(true));
    }

    // Container is neither string nor array.
    return ScriptResult::success(JsValue::make_boolean(false));
}

} // namespace

void register_includes_func(StdLibrary &lib) {
    lib.register_func("includes", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = true},
                      &includes_fn, nullptr, "includes");
}

} // namespace fiber::script::std_lib
