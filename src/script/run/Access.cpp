#include "Access.h"

#include "../../common/json/Utf.h"
#include "../Runtime.h"

#include <string>

namespace fiber::script::run {

namespace {

ScriptAbortReason heap_required_error() { return ScriptAbortReason::InvalidState; }

ScriptAbortReason oom_error() { return ScriptAbortReason::OutOfMemory; }

ScriptAbortReason index_error(std::string message) {
    (void) message;
    return ScriptAbortReason::IndexError;
}

bool get_index(const fiber::json::JsValue &key, std::int64_t &out) {
    if (fiber::json::js_value_type(key) == fiber::json::JsNodeType::Integer) {
        out = fiber::json::js_value_int64(key);
        return true;
    }
    return false;
}

fiber::json::GcString *ensure_heap_string(ScriptRuntime &runtime, const fiber::json::JsValue &value,
                                          ScriptAbortReason &error) {
    if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::String &&
        !fiber::json::js_value_is_borrowed_string(value)) {
        return fiber::json::js_value_heap_ptr<fiber::json::GcString>(const_cast<fiber::json::JsValue &>(value));
    }
    if (fiber::json::js_value_type(value) != fiber::json::JsNodeType::String ||
        !fiber::json::js_value_is_borrowed_string(value)) {
        return nullptr;
    }
    fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
    fiber::json::GcString *str = runtime.alloc_with_gc(fiber::json::gc_estimate_utf8_string_bytes(native.len), [&]() {
        return fiber::json::gc_new_string(&runtime.heap(), native.data, native.len);
    });
    if (!str) {
        error = oom_error();
        return nullptr;
    }
    return str;
}

ScriptResult make_heap_string_value(fiber::json::GcString *str) {
    return str ? fiber::json::js_make_heap_ref(&str->hdr, fiber::json::JsHeapKind::String)
               : fiber::json::JsValue::make_undefined();
}

bool string_length(const fiber::json::JsValue &value, std::size_t &out, ScriptAbortReason &error) {
    if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::String &&
        !fiber::json::js_value_is_borrowed_string(value)) {
        auto *str = fiber::json::js_value_heap_ptr<const fiber::json::GcString>(value);
        out = str ? str->len : 0;
        return true;
    }
    if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::String &&
        fiber::json::js_value_is_borrowed_string(value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
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

ScriptResult string_char_at(ScriptRuntime &runtime, const fiber::json::JsValue &value, std::int64_t index) {
    if (index < 0) {
        return fiber::json::JsValue::make_undefined();
    }
    fiber::json::GcString *str = nullptr;
    fiber::json::JsValue rooted_str = fiber::json::JsValue::make_undefined();
    TempRootScope temp_roots(runtime);
    temp_roots.add(&rooted_str);
    if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::String &&
        !fiber::json::js_value_is_borrowed_string(value)) {
        str = fiber::json::js_value_heap_ptr<fiber::json::GcString>(const_cast<fiber::json::JsValue &>(value));
    } else if (fiber::json::js_value_type(value) == fiber::json::JsNodeType::String &&
               fiber::json::js_value_is_borrowed_string(value)) {
        fiber::json::NativeStr native = fiber::json::js_value_native_string(value);
        str = runtime.alloc_with_gc(fiber::json::gc_estimate_utf8_string_bytes(native.len), [&]() {
            return fiber::json::gc_new_string(&runtime.heap(), native.data, native.len);
        });
    }
    if (!str) {
        return ScriptResult::abort(oom_error());
    }
    rooted_str = fiber::json::js_make_heap_ref(&str->hdr, fiber::json::JsHeapKind::String);
    if (index >= static_cast<std::int64_t>(str->len)) {
        return fiber::json::JsValue::make_undefined();
    }
    if (str->encoding == fiber::json::GcStringEncoding::Byte) {
        std::uint8_t byte = str->data8[index];
        fiber::json::GcString *out =
                runtime.alloc_with_gc(fiber::json::gc_estimate_string_bytes(1, fiber::json::GcStringEncoding::Byte),
                                      [&]() { return fiber::json::gc_new_string_bytes(&runtime.heap(), &byte, 1); });
        if (!out) {
            return ScriptResult::abort(oom_error());
        }
        return make_heap_string_value(out);
    }
    char16_t unit = str->data16[index];
    fiber::json::GcString *out =
            runtime.alloc_with_gc(fiber::json::gc_estimate_string_bytes(1, fiber::json::GcStringEncoding::Utf16),
                                  [&]() { return fiber::json::gc_new_string_utf16(&runtime.heap(), &unit, 1); });
    if (!out) {
        return ScriptResult::abort(oom_error());
    }
    return make_heap_string_value(out);
}

} // namespace

ScriptResult Access::expand_object(const fiber::json::JsValue &target, const fiber::json::JsValue &addition,
                                   ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(target) != fiber::json::JsNodeType::Object ||
        fiber::json::js_value_type(addition) != fiber::json::JsNodeType::Object) {
        return target;
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    auto *target_obj =
            fiber::json::js_value_heap_ptr<fiber::json::GcObject>(const_cast<fiber::json::JsValue &>(target));
    auto *add_obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(addition);
    if (!target_obj || !add_obj) {
        return target;
    }
    std::size_t expected = target_obj->size + add_obj->size;
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_object_growth_bytes(target_obj, expected),
                                   [&]() { return fiber::json::gc_object_reserve(heap, target_obj, expected); })) {
        return ScriptResult::abort(oom_error());
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied || !entry->key) {
            continue;
        }
        if (!fiber::json::gc_object_set(heap, target_obj, entry->key, entry->value)) {
            return ScriptResult::abort(oom_error());
        }
    }
    return target;
}

ScriptResult Access::expand_array(const fiber::json::JsValue &target, const fiber::json::JsValue &addition,
                                  ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(target) != fiber::json::JsNodeType::Array) {
        return target;
    }
    if (fiber::json::js_value_type(addition) != fiber::json::JsNodeType::Array &&
        fiber::json::js_value_type(addition) != fiber::json::JsNodeType::Object) {
        return target;
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    auto *target_arr = fiber::json::js_value_heap_ptr<fiber::json::GcArray>(const_cast<fiber::json::JsValue &>(target));
    if (!target_arr) {
        return target;
    }
    if (fiber::json::js_value_type(addition) == fiber::json::JsNodeType::Array) {
        auto *add_arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(addition);
        if (!add_arr) {
            return target;
        }
        std::size_t expected = target_arr->size + add_arr->size;
        if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_array_growth_bytes(target_arr, expected),
                                       [&]() { return fiber::json::gc_array_reserve(heap, target_arr, expected); })) {
            return ScriptResult::abort(oom_error());
        }
        for (std::size_t i = 0; i < add_arr->size; ++i) {
            const fiber::json::JsValue *item = fiber::json::gc_array_get(add_arr, i);
            if (!fiber::json::gc_array_push(heap, target_arr, item ? *item : fiber::json::JsValue::make_undefined())) {
                return ScriptResult::abort(oom_error());
            }
        }
        return target;
    }
    auto *add_obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(addition);
    if (!add_obj) {
        return target;
    }
    std::size_t expected = target_arr->size + add_obj->size;
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_array_growth_bytes(target_arr, expected),
                                   [&]() { return fiber::json::gc_array_reserve(heap, target_arr, expected); })) {
        return ScriptResult::abort(oom_error());
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied) {
            continue;
        }
        if (!fiber::json::gc_array_push(heap, target_arr, entry->value)) {
            return ScriptResult::abort(oom_error());
        }
    }
    return target;
}

ScriptResult Access::push_array(const fiber::json::JsValue &target, const fiber::json::JsValue &addition,
                                ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(target) != fiber::json::JsNodeType::Array) {
        return target;
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    auto *arr = fiber::json::js_value_heap_ptr<fiber::json::GcArray>(const_cast<fiber::json::JsValue &>(target));
    if (!arr) {
        return target;
    }
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_array_growth_bytes(arr, arr->size + 1),
                                   [&]() { return fiber::json::gc_array_push(heap, arr, addition); })) {
        return ScriptResult::abort(oom_error());
    }
    return target;
}

ScriptResult Access::index_get(const fiber::json::JsValue &parent, const fiber::json::JsValue &key,
                               ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return fiber::json::JsValue::make_undefined();
        }
        if (idx < 0) {
            return fiber::json::JsValue::make_undefined();
        }
        auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(parent);
        const fiber::json::JsValue *found =
                arr ? fiber::json::gc_array_get(arr, static_cast<std::size_t>(idx)) : nullptr;
        return found ? *found : fiber::json::JsValue::make_undefined();
    }
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Object) {
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return ScriptResult::abort(error);
        }
        if (!key_str) {
            return fiber::json::JsValue::make_undefined();
        }
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(parent);
        const fiber::json::JsValue *found = obj ? fiber::json::gc_object_get(obj, key_str) : nullptr;
        return found ? *found : fiber::json::JsValue::make_undefined();
    }
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::String) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return fiber::json::JsValue::make_undefined();
        }
        return string_char_at(runtime, parent, idx);
    }
    return fiber::json::JsValue::make_undefined();
}

ScriptResult Access::index_set(const fiber::json::JsValue &parent, const fiber::json::JsValue &key,
                               const fiber::json::JsValue &value, ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return ScriptResult::abort(index_error("array index must be integer"));
        }
        auto *arr = fiber::json::js_value_heap_ptr<fiber::json::GcArray>(const_cast<fiber::json::JsValue &>(parent));
        if (!arr || idx < 0 || idx >= static_cast<std::int64_t>(arr->size)) {
            return ScriptResult::abort(index_error("array index out of bounds"));
        }
        fiber::json::GcHeap *heap = &runtime.heap();
        if (!runtime.run_with_gc_retry(
                    fiber::json::gc_estimate_array_growth_bytes(arr, static_cast<std::size_t>(idx) + 1),
                    [&]() { return fiber::json::gc_array_set(heap, arr, static_cast<std::size_t>(idx), value); })) {
            return ScriptResult::abort(oom_error());
        }
        return value;
    }
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Object) {
        fiber::json::GcHeap *heap = &runtime.heap();
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return ScriptResult::abort(error);
        }
        if (!key_str) {
            return ScriptResult::abort(index_error("object key must be string"));
        }
        fiber::json::JsValue rooted_key = fiber::json::JsValue::make_undefined();
        TempRootScope temp_roots(runtime);
        temp_roots.add(&rooted_key);
        rooted_key = fiber::json::js_make_heap_ref(&key_str->hdr, fiber::json::JsHeapKind::String);
        auto *obj = fiber::json::js_value_heap_ptr<fiber::json::GcObject>(const_cast<fiber::json::JsValue &>(parent));
        if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_object_growth_bytes(obj, obj->size + 1),
                                       [&]() { return fiber::json::gc_object_set(heap, obj, key_str, value); })) {
            return ScriptResult::abort(oom_error());
        }
        return value;
    }
    return ScriptResult::abort(index_error("indexing not supported"));
}

ScriptResult Access::index_set1(const fiber::json::JsValue &parent, const fiber::json::JsValue &key,
                                const fiber::json::JsValue &value, ScriptRuntime &runtime) {
    ScriptResult result = index_set(parent, key, value, runtime);
    if (!result) {
        return result;
    }
    return parent;
}

ScriptResult Access::prop_get(const fiber::json::JsValue &parent, const fiber::json::JsValue &key,
                              ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Object) {
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return ScriptResult::abort(error);
        }
        if (!key_str) {
            return fiber::json::JsValue::make_undefined();
        }
        auto *obj = fiber::json::js_value_heap_ptr<const fiber::json::GcObject>(parent);
        const fiber::json::JsValue *found = obj ? fiber::json::gc_object_get(obj, key_str) : nullptr;
        return found ? *found : fiber::json::JsValue::make_undefined();
    }
    if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Array ||
        fiber::json::js_value_type(parent) == fiber::json::JsNodeType::String) {
        std::size_t len = 0;
        ScriptAbortReason error = ScriptAbortReason::None;
        if (fiber::json::js_value_type(parent) == fiber::json::JsNodeType::Array) {
            auto *arr = fiber::json::js_value_heap_ptr<const fiber::json::GcArray>(parent);
            len = arr ? arr->size : 0;
        } else if (!string_length(parent, len, error)) {
            return ScriptResult::abort(error);
        }
        return fiber::json::JsValue::make_integer(static_cast<std::int64_t>(len));
    }
    (void) key;
    return fiber::json::JsValue::make_undefined();
}

ScriptResult Access::prop_set(const fiber::json::JsValue &parent, const fiber::json::JsValue &value,
                              const fiber::json::JsValue &key, ScriptRuntime &runtime) {
    if (fiber::json::js_value_type(parent) != fiber::json::JsNodeType::Object) {
        return ScriptResult::abort(index_error("property set not supported"));
    }
    fiber::json::GcHeap *heap = &runtime.heap();
    ScriptAbortReason error = ScriptAbortReason::None;
    fiber::json::GcString *key_str = ensure_heap_string(runtime, key, error);
    if (!key_str && error != ScriptAbortReason::None) {
        return ScriptResult::abort(error);
    }
    if (!key_str) {
        return ScriptResult::abort(index_error("property key must be string"));
    }
    fiber::json::JsValue rooted_key = fiber::json::JsValue::make_undefined();
    TempRootScope temp_roots(runtime);
    temp_roots.add(&rooted_key);
    rooted_key = fiber::json::js_make_heap_ref(&key_str->hdr, fiber::json::JsHeapKind::String);
    auto *obj = fiber::json::js_value_heap_ptr<fiber::json::GcObject>(const_cast<fiber::json::JsValue &>(parent));
    if (!runtime.run_with_gc_retry(fiber::json::gc_estimate_object_growth_bytes(obj, obj->size + 1),
                                   [&]() { return fiber::json::gc_object_set(heap, obj, key_str, value); })) {
        return ScriptResult::abort(oom_error());
    }
    return value;
}

ScriptResult Access::prop_set1(const fiber::json::JsValue &parent, const fiber::json::JsValue &value,
                               const fiber::json::JsValue &key, ScriptRuntime &runtime) {
    ScriptResult result = prop_set(parent, value, key, runtime);
    if (!result) {
        return result;
    }
    return parent;
}

} // namespace fiber::script::run
