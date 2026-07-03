#include "Access.h"

#include "../../common/json/Utf.h"

#include <string>

namespace fiber::script::run {

namespace {

ScriptAbortReason oom_error() { return ScriptAbortReason::OutOfMemory; }

ScriptAbortReason index_error(std::string message) {
    (void) message;
    return ScriptAbortReason::IndexError;
}

ScriptStatus set_value(ValueHandle out, const fiber::json::JsValue &value) noexcept {
    if (!out) {
        return ScriptStatus::abort(oom_error());
    }
    *out = value;
    return ScriptStatus::success();
}

ScriptStatus set_undefined(ValueHandle out) noexcept { return set_value(out, fiber::json::JsValue::make_undefined()); }

bool get_index(ValueHandle key, std::int64_t &out) noexcept {
    if (key && fiber::json::js_value_type(*key) == fiber::json::JsNodeType::Integer) {
        out = fiber::json::js_value_int64(*key);
        return true;
    }
    return false;
}

fiber::json::GcString *ensure_heap_string(ScriptRuntime &runtime, ValueHandle value, ScriptAbortReason &error) {
    if (!value) {
        return nullptr;
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::String &&
        !fiber::json::js_value_is_borrowed_string(*value)) {
        return fiber::json::js_value_heap_ptr<fiber::json::GcString>(*value);
    }
    if (fiber::json::js_value_type(*value) != fiber::json::JsNodeType::String ||
        !fiber::json::js_value_is_borrowed_string(*value)) {
        return nullptr;
    }
    fiber::json::NativeStr native = fiber::json::js_value_native_string(*value);
    fiber::json::GcString *str = runtime.alloc_with_gc(fiber::json::gc_estimate_utf8_string_bytes(native.len), [&]() {
        return fiber::json::gc_new_string(&runtime.heap(), native.data, native.len);
    });
    if (!str) {
        error = oom_error();
        return nullptr;
    }
    return str;
}

fiber::json::JsValue make_heap_string_value(fiber::json::GcString *str) {
    return str ? fiber::json::js_make_heap_ref(&str->hdr, fiber::json::JsHeapKind::String)
               : fiber::json::JsValue::make_undefined();
}

bool string_length(ValueHandle value, std::size_t &out, ScriptAbortReason &error) {
    if (!value) {
        return false;
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::String &&
        !fiber::json::js_value_is_borrowed_string(*value)) {
        auto *str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(*value);
        out = str ? str->len : 0;
        return true;
    }
    if (fiber::json::js_value_type(*value) == fiber::json::JsNodeType::String &&
        fiber::json::js_value_is_borrowed_string(*value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(*value);
        fiber::json::Utf8ScanResult scan;
        if (!fiber::json::utf8_scan(native.data, native.len, scan)) {
            error = ScriptAbortReason::InvalidArgument;
            return false;
        }
        out = scan.utf16_len;
        return true;
    }
    return false;
}

ScriptStatus string_char_at(ScriptRuntime &runtime, ValueHandle out, ValueHandle value, std::int64_t index) {
    if (index < 0) {
        return set_undefined(out);
    }
    ScriptRuntime::LocalMark mark(runtime);
    fiber::json::GcString *str = nullptr;
    if (value && fiber::json::js_value_type(*value) == fiber::json::JsNodeType::String &&
        !fiber::json::js_value_is_borrowed_string(*value)) {
        str = fiber::json::js_value_heap_ptr<fiber::json::GcString>(*value);
    } else if (value && fiber::json::js_value_type(*value) == fiber::json::JsNodeType::String &&
               fiber::json::js_value_is_borrowed_string(*value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(*value);
        str = runtime.alloc_with_gc(fiber::json::gc_estimate_utf8_string_bytes(native.len), [&]() {
            return fiber::json::gc_new_string(&runtime.heap(), native.data, native.len);
        });
    }
    if (!str) {
        return ScriptStatus::abort(oom_error());
    }
    ValueHandle rooted_str = runtime.local_value();
    if (!rooted_str) {
        return ScriptStatus::abort(oom_error());
    }
    *rooted_str = make_heap_string_value(str);
    if (index >= static_cast<std::int64_t>(str->len)) {
        return set_undefined(out);
    }
    if (str->encoding == fiber::json::GcStringEncoding::Byte) {
        std::uint8_t byte = str->data8[index];
        fiber::json::GcString *result =
                runtime.alloc_with_gc(fiber::json::gc_estimate_string_bytes(1, fiber::json::GcStringEncoding::Byte),
                                      [&]() { return fiber::json::gc_new_string_bytes(&runtime.heap(), &byte, 1); });
        if (!result) {
            return ScriptStatus::abort(oom_error());
        }
        return set_value(out, make_heap_string_value(result));
    }
    char16_t unit = str->data16[index];
    fiber::json::GcString *result =
            runtime.alloc_with_gc(fiber::json::gc_estimate_string_bytes(1, fiber::json::GcStringEncoding::Utf16),
                                  [&]() { return fiber::json::gc_new_string_utf16(&runtime.heap(), &unit, 1); });
    if (!result) {
        return ScriptStatus::abort(oom_error());
    }
    return set_value(out, make_heap_string_value(result));
}

} // namespace

ScriptStatus Access::expand_object(ScriptRuntime &runtime, ValueHandle out, ValueHandle target,
                                   ValueHandle addition) noexcept {
    if (!target || !addition || fiber::json::js_value_type(*target) != fiber::json::JsNodeType::Object ||
        fiber::json::js_value_type(*addition) != fiber::json::JsNodeType::Object) {
        return set_value(out, target ? *target : fiber::json::JsValue::make_undefined());
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    auto *target_obj = fiber::json::js_value_heap_ptr<fiber::json::GcObject>(*target);
    auto *add_obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*addition);
    if (!target_obj || !add_obj) {
        return set_value(out, *target);
    }
    std::size_t expected = target_obj->size + add_obj->size;
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_object_growth_bytes(target_obj, expected),
                                   [&]() { return fiber::json::gc_object_reserve(heap, target_obj, expected); })) {
        return ScriptStatus::abort(oom_error());
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied || !entry->key) {
            continue;
        }
        if (!fiber::json::gc_object_set(heap, target_obj, entry->key, entry->value)) {
            return ScriptStatus::abort(oom_error());
        }
    }
    return set_value(out, *target);
}

ScriptStatus Access::expand_array(ScriptRuntime &runtime, ValueHandle out, ValueHandle target,
                                  ValueHandle addition) noexcept {
    if (!target || fiber::json::js_value_type(*target) != fiber::json::JsNodeType::Array) {
        return set_value(out, target ? *target : fiber::json::JsValue::make_undefined());
    }
    if (!addition || (fiber::json::js_value_type(*addition) != fiber::json::JsNodeType::Array &&
                      fiber::json::js_value_type(*addition) != fiber::json::JsNodeType::Object)) {
        return set_value(out, *target);
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    auto *target_arr = fiber::json::js_value_heap_ptr<fiber::json::GcArray>(*target);
    if (!target_arr) {
        return set_value(out, *target);
    }
    if (fiber::json::js_value_type(*addition) == fiber::json::JsNodeType::Array) {
        auto *add_arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(*addition);
        if (!add_arr) {
            return set_value(out, *target);
        }
        std::size_t expected = target_arr->size + add_arr->size;
        if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_array_growth_bytes(target_arr, expected),
                                       [&]() { return fiber::json::gc_array_reserve(heap, target_arr, expected); })) {
            return ScriptStatus::abort(oom_error());
        }
        for (std::size_t i = 0; i < add_arr->size; ++i) {
            const fiber::json::JsValue *item = fiber::json::gc_array_get(add_arr, i);
            if (!fiber::json::gc_array_push(heap, target_arr, item ? *item : fiber::json::JsValue::make_undefined())) {
                return ScriptStatus::abort(oom_error());
            }
        }
        return set_value(out, *target);
    }
    auto *add_obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*addition);
    if (!add_obj) {
        return set_value(out, *target);
    }
    std::size_t expected = target_arr->size + add_obj->size;
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_array_growth_bytes(target_arr, expected),
                                   [&]() { return fiber::json::gc_array_reserve(heap, target_arr, expected); })) {
        return ScriptStatus::abort(oom_error());
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied) {
            continue;
        }
        if (!fiber::json::gc_array_push(heap, target_arr, entry->value)) {
            return ScriptStatus::abort(oom_error());
        }
    }
    return set_value(out, *target);
}

ScriptStatus Access::push_array(ScriptRuntime &runtime, ValueHandle out, ValueHandle target,
                                ValueHandle addition) noexcept {
    if (!target || fiber::json::js_value_type(*target) != fiber::json::JsNodeType::Array) {
        return set_value(out, target ? *target : fiber::json::JsValue::make_undefined());
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    auto *arr = fiber::json::js_value_heap_ptr<fiber::json::GcArray>(*target);
    if (!arr) {
        return set_value(out, *target);
    }
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_array_growth_bytes(arr, arr->size + 1),
                                   [&]() { return fiber::json::gc_array_push(heap, arr, *addition); })) {
        return ScriptStatus::abort(oom_error());
    }
    return set_value(out, *target);
}

ScriptStatus Access::index_get(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key) noexcept {
    if (!parent) {
        return set_undefined(out);
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx) || idx < 0) {
            return set_undefined(out);
        }
        auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(*parent);
        const fiber::json::JsValue *found =
                arr ? fiber::json::gc_array_get(arr, static_cast<std::size_t>(idx)) : nullptr;
        return set_value(out, found ? *found : fiber::json::JsValue::make_undefined());
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Object) {
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return ScriptStatus::abort(error);
        }
        if (!key_str) {
            return set_undefined(out);
        }
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*parent);
        const fiber::json::JsValue *found = obj ? fiber::json::gc_object_get(obj, key_str) : nullptr;
        return set_value(out, found ? *found : fiber::json::JsValue::make_undefined());
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::String) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return set_undefined(out);
        }
        return string_char_at(runtime, out, parent, idx);
    }
    return set_undefined(out);
}

ScriptStatus Access::index_set(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key,
                               ValueHandle value) noexcept {
    if (!parent || !value) {
        return ScriptStatus::abort(index_error("indexing not supported"));
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return ScriptStatus::abort(index_error("array index must be integer"));
        }
        auto *arr = fiber::json::js_value_heap_ptr<fiber::json::GcArray>(*parent);
        if (!arr || idx < 0 || idx >= static_cast<std::int64_t>(arr->size)) {
            return ScriptStatus::abort(index_error("array index out of bounds"));
        }
        fiber::json::GcHeap *heap = &runtime.heap();
        if (!runtime.run_with_gc_retry(
                    fiber::json::gc_estimate_array_growth_bytes(arr, static_cast<std::size_t>(idx) + 1),
                    [&]() { return fiber::json::gc_array_set(heap, arr, static_cast<std::size_t>(idx), *value); })) {
            return ScriptStatus::abort(oom_error());
        }
        return set_value(out, *value);
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Object) {
        ScriptRuntime::LocalMark mark(runtime);
        fiber::json::GcHeap *heap = &runtime.heap();
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return ScriptStatus::abort(error);
        }
        if (!key_str) {
            return ScriptStatus::abort(index_error("object key must be string"));
        }
        ValueHandle rooted_key = runtime.local_value();
        if (!rooted_key) {
            return ScriptStatus::abort(oom_error());
        }
        *rooted_key = make_heap_string_value(key_str);
        auto *obj = fiber::json::js_value_heap_ptr<fiber::json::GcObject>(*parent);
        if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_object_growth_bytes(obj, obj->size + 1),
                                       [&]() { return fiber::json::gc_object_set(heap, obj, key_str, *value); })) {
            return ScriptStatus::abort(oom_error());
        }
        return set_value(out, *value);
    }
    return ScriptStatus::abort(index_error("indexing not supported"));
}

ScriptStatus Access::index_set1(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key,
                                ValueHandle value) noexcept {
    ScriptRuntime::LocalMark mark(runtime);
    ValueHandle tmp = runtime.local_value();
    if (!tmp) {
        return ScriptStatus::abort(oom_error());
    }
    ScriptStatus status = index_set(runtime, tmp, parent, key, value);
    if (!status) {
        return status;
    }
    return set_value(out, parent ? *parent : fiber::json::JsValue::make_undefined());
}

ScriptStatus Access::prop_get(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle key) noexcept {
    if (!parent) {
        return set_undefined(out);
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Object) {
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return ScriptStatus::abort(error);
        }
        if (!key_str) {
            return set_undefined(out);
        }
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(*parent);
        const fiber::json::JsValue *found = obj ? fiber::json::gc_object_get(obj, key_str) : nullptr;
        return set_value(out, found ? *found : fiber::json::JsValue::make_undefined());
    }
    if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Array ||
        fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::String) {
        std::size_t len = 0;
        ScriptAbortReason error = ScriptAbortReason::None;
        if (fiber::json::js_value_type(*parent) == fiber::json::JsNodeType::Array) {
            auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(*parent);
            len = arr ? arr->size : 0;
        } else if (!string_length(parent, len, error)) {
            return ScriptStatus::abort(error);
        }
        return set_value(out, fiber::json::JsValue::make_integer(static_cast<std::int64_t>(len)));
    }
    (void) key;
    return set_undefined(out);
}

ScriptStatus Access::prop_set(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle value,
                              ValueHandle key) noexcept {
    if (!parent || fiber::json::js_value_type(*parent) != fiber::json::JsNodeType::Object) {
        return ScriptStatus::abort(index_error("property set not supported"));
    }
    ScriptRuntime::LocalMark mark(runtime);
    fiber::json::GcHeap *heap = &runtime.heap();
    ScriptAbortReason error = ScriptAbortReason::None;
    fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
    if (!key_str && error != ScriptAbortReason::None) {
        return ScriptStatus::abort(error);
    }
    if (!key_str) {
        return ScriptStatus::abort(index_error("property key must be string"));
    }
    ValueHandle rooted_key = runtime.local_value();
    if (!rooted_key) {
        return ScriptStatus::abort(oom_error());
    }
    *rooted_key = make_heap_string_value(key_str);
    auto *obj = fiber::json::js_value_heap_ptr<fiber::json::GcObject>(*parent);
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_object_growth_bytes(obj, obj->size + 1),
                                   [&]() { return fiber::json::gc_object_set(heap, obj, key_str, *value); })) {
        return ScriptStatus::abort(oom_error());
    }
    return set_value(out, *value);
}

ScriptStatus Access::prop_set1(ScriptRuntime &runtime, ValueHandle out, ValueHandle parent, ValueHandle value,
                               ValueHandle key) noexcept {
    ScriptRuntime::LocalMark mark(runtime);
    ValueHandle tmp = runtime.local_value();
    if (!tmp) {
        return ScriptStatus::abort(oom_error());
    }
    ScriptStatus status = prop_set(runtime, tmp, parent, value, key);
    if (!status) {
        return status;
    }
    return set_value(out, parent ? *parent : fiber::json::JsValue::make_undefined());
}

} // namespace fiber::script::run
