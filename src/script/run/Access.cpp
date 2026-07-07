#include "Access.h"

#include "../../common/json/Utf.h"

#include <string>

namespace fiber::script::run {

namespace {

ScriptAbortReason oom_error() { return ScriptAbortReason::OutOfMemory; }

fiber::script::JsValue &mutable_value(ConstValueHandle value) noexcept {
    return *const_cast<fiber::script::JsValue *>(value.get());
}

template<typename T>
T *mutable_heap_ptr(ConstValueHandle value) noexcept {
    if (!value) {
        return nullptr;
    }
    return fiber::script::js_value_heap_ptr<T>(mutable_value(value));
}

bool get_index(ConstValueHandle key, std::int64_t &out) noexcept {
    if (key && fiber::script::js_value_type(*key) == fiber::script::JsNodeType::Integer) {
        out = fiber::script::js_value_int64(*key);
        return true;
    }
    return false;
}

// OOM is the only abort this can signal (allocation failure while interning a borrowed string); a
// non-string key returns nullptr with error left None and the caller decides undefined (read) vs
// TypeError (write).
fiber::script::GcString *ensure_heap_string(ScriptRuntime &runtime, ConstValueHandle value, ScriptAbortReason &error) {
    if (!value) {
        return nullptr;
    }
    if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
        !fiber::script::js_value_is_borrowed_string(*value)) {
        return const_cast<fiber::script::GcString *>(
                fiber::script::js_value_heap_ptr<const fiber::script::GcString>(*value));
    }
    if (fiber::script::js_value_type(*value) != fiber::script::JsNodeType::String ||
        !fiber::script::js_value_is_borrowed_string(*value)) {
        return nullptr;
    }
    fiber::script::NativeStr native = fiber::script::js_value_native_string(*value);
    fiber::script::GcString *str =
            runtime.alloc_with_gc(fiber::script::gc_estimate_utf8_string_bytes(native.len), [&]() {
                return fiber::script::gc_new_string(&runtime.heap(), native.data, native.len);
            });
    if (!str) {
        error = oom_error();
        return nullptr;
    }
    return str;
}

fiber::script::JsValue make_heap_string_value(fiber::script::GcString *str) {
    return str ? fiber::script::js_make_heap_ref(&str->hdr, fiber::script::JsHeapKind::String)
               : fiber::script::JsValue::make_undefined();
}

// Malformed UTF-8 fails (returns false); the read caller folds that to undefined.
bool string_length(ConstValueHandle value, std::size_t &out) {
    if (!value) {
        return false;
    }
    if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
        !fiber::script::js_value_is_borrowed_string(*value)) {
        auto *str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(*value);
        out = str ? str->len : 0;
        return true;
    }
    if (fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
        fiber::script::js_value_is_borrowed_string(*value)) {
        fiber::script::NativeStr native = fiber::script::js_value_native_string(*value);
        fiber::json::Utf8ScanResult scan;
        if (!fiber::json::utf8_scan(native.data, native.len, scan)) {
            return false;
        }
        out = scan.utf16_len;
        return true;
    }
    return false;
}

CallResult string_char_at(ScriptRuntime &runtime, ResultPayload &result, ConstValueHandle value, std::int64_t index) {
    if (index < 0) {
        return set_undefined(result);
    }
    ScriptRuntime::LocalMark mark(runtime);
    fiber::script::GcString *str = nullptr;
    if (value && fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
        !fiber::script::js_value_is_borrowed_string(*value)) {
        str = const_cast<fiber::script::GcString *>(
                fiber::script::js_value_heap_ptr<const fiber::script::GcString>(*value));
    } else if (value && fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
               fiber::script::js_value_is_borrowed_string(*value)) {
        fiber::script::NativeStr native = fiber::script::js_value_native_string(*value);
        str = runtime.alloc_with_gc(fiber::script::gc_estimate_utf8_string_bytes(native.len), [&]() {
            return fiber::script::gc_new_string(&runtime.heap(), native.data, native.len);
        });
    }
    if (!str) {
        return set_abort(result, oom_error());
    }
    ValueHandle rooted_str = runtime.local_value();
    if (!rooted_str) {
        return set_abort(result, oom_error());
    }
    *rooted_str = make_heap_string_value(str);
    if (index >= static_cast<std::int64_t>(str->len)) {
        return set_undefined(result);
    }
    if (str->encoding == fiber::script::GcStringEncoding::Byte) {
        std::uint8_t byte = str->data8[index];
        fiber::script::GcString *char_str =
                runtime.alloc_with_gc(fiber::script::gc_estimate_string_bytes(1, fiber::script::GcStringEncoding::Byte),
                                      [&]() { return fiber::script::gc_new_string_bytes(&runtime.heap(), &byte, 1); });
        if (!char_str) {
            return set_abort(result, oom_error());
        }
        return set_value(result, make_heap_string_value(char_str));
    }
    char16_t unit = str->data16[index];
    fiber::script::GcString *char_str =
            runtime.alloc_with_gc(fiber::script::gc_estimate_string_bytes(1, fiber::script::GcStringEncoding::Utf16),
                                  [&]() { return fiber::script::gc_new_string_utf16(&runtime.heap(), &unit, 1); });
    if (!char_str) {
        return set_abort(result, oom_error());
    }
    return set_value(result, make_heap_string_value(char_str));
}

} // namespace

CallResult Access::expand_object(ScriptRuntime &runtime, ConstValueHandle target, ConstValueHandle addition,
                                 ResultPayload &result) noexcept {
    if (!target || !addition || fiber::script::js_value_type(*target) != fiber::script::JsNodeType::Object ||
        fiber::script::js_value_type(*addition) != fiber::script::JsNodeType::Object) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    fiber::script::GcHeap *heap = &runtime.heap();
    auto *target_obj = mutable_heap_ptr<fiber::script::GcObject>(target);
    auto *add_obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*addition);
    if (!target_obj || !add_obj) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    std::size_t expected = target_obj->size + add_obj->size;
    if (!runtime.run_with_gc_retry(fiber::script::gc_estimate_object_growth_bytes(target_obj, expected),
                                   [&]() { return fiber::script::gc_object_reserve(heap, target_obj, expected); })) {
        return set_abort(result, oom_error());
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied || !entry->key) {
            continue;
        }
        if (!fiber::script::gc_object_set(heap, target_obj, entry->key, entry->value)) {
            return set_abort(result, oom_error());
        }
    }
    return set_value(result, *target);
}

CallResult Access::expand_array(ScriptRuntime &runtime, ConstValueHandle target, ConstValueHandle addition,
                                ResultPayload &result) noexcept {
    if (!target || fiber::script::js_value_type(*target) != fiber::script::JsNodeType::Array) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (!addition || (fiber::script::js_value_type(*addition) != fiber::script::JsNodeType::Array &&
                      fiber::script::js_value_type(*addition) != fiber::script::JsNodeType::Object)) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    fiber::script::GcHeap *heap = &runtime.heap();
    auto *target_arr = mutable_heap_ptr<fiber::script::GcArray>(target);
    if (!target_arr) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(*addition) == fiber::script::JsNodeType::Array) {
        auto *add_arr = fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(*addition);
        if (!add_arr) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        std::size_t expected = target_arr->size + add_arr->size;
        if (!runtime.run_with_gc_retry(fiber::script::gc_estimate_array_growth_bytes(target_arr, expected),
                                       [&]() { return fiber::script::gc_array_reserve(heap, target_arr, expected); })) {
            return set_abort(result, oom_error());
        }
        for (std::size_t i = 0; i < add_arr->size; ++i) {
            const fiber::script::JsValue *item = fiber::script::gc_array_get(add_arr, i);
            if (!fiber::script::gc_array_push(heap, target_arr,
                                              item ? *item : fiber::script::JsValue::make_undefined())) {
                return set_abort(result, oom_error());
            }
        }
        return set_value(result, *target);
    }
    auto *add_obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*addition);
    if (!add_obj) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    std::size_t expected = target_arr->size + add_obj->size;
    if (!runtime.run_with_gc_retry(fiber::script::gc_estimate_array_growth_bytes(target_arr, expected),
                                   [&]() { return fiber::script::gc_array_reserve(heap, target_arr, expected); })) {
        return set_abort(result, oom_error());
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied) {
            continue;
        }
        if (!fiber::script::gc_array_push(heap, target_arr, entry->value)) {
            return set_abort(result, oom_error());
        }
    }
    return set_value(result, *target);
}

CallResult Access::push_array(ScriptRuntime &runtime, ConstValueHandle target, ConstValueHandle addition,
                              ResultPayload &result) noexcept {
    if (!target || fiber::script::js_value_type(*target) != fiber::script::JsNodeType::Array) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    fiber::script::GcHeap *heap = &runtime.heap();
    auto *arr = mutable_heap_ptr<fiber::script::GcArray>(target);
    if (!arr) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (!runtime.run_with_gc_retry(fiber::script::gc_estimate_array_growth_bytes(arr, arr->size + 1), [&]() {
            return fiber::script::gc_array_push(heap, arr,
                                                addition ? *addition : fiber::script::JsValue::make_undefined());
        })) {
        return set_abort(result, oom_error());
    }
    return set_value(result, *target);
}

CallResult Access::index_get(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                             ResultPayload &result) noexcept {
    if (!parent) {
        return set_undefined(result);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx) || idx < 0) {
            return set_undefined(result);
        }
        auto *arr = fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(*parent);
        const fiber::script::JsValue *found =
                arr ? fiber::script::gc_array_get(arr, static_cast<std::size_t>(idx)) : nullptr;
        return set_value(result, found ? *found : fiber::script::JsValue::make_undefined());
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Object) {
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::script::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return set_abort(result, error);
        }
        if (!key_str) {
            return set_undefined(result);
        }
        auto *obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*parent);
        const fiber::script::JsValue *found = obj ? fiber::script::gc_object_get(obj, key_str) : nullptr;
        return set_value(result, found ? *found : fiber::script::JsValue::make_undefined());
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::String) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return set_undefined(result);
        }
        return string_char_at(runtime, result, parent, idx);
    }
    return set_undefined(result);
}

CallResult Access::index_set(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                             ConstValueHandle value, ResultPayload &result) noexcept {
    if (!parent || !value) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        auto *arr = mutable_heap_ptr<fiber::script::GcArray>(parent);
        if (!arr || idx < 0 || idx >= static_cast<std::int64_t>(arr->size)) {
            return set_exception(result, fiber::script::ExceptionKind::RangeError);
        }
        fiber::script::GcHeap *heap = &runtime.heap();
        if (!runtime.run_with_gc_retry(
                    fiber::script::gc_estimate_array_growth_bytes(arr, static_cast<std::size_t>(idx) + 1),
                    [&]() { return fiber::script::gc_array_set(heap, arr, static_cast<std::size_t>(idx), *value); })) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *value);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Object) {
        ScriptRuntime::LocalMark mark(runtime);
        fiber::script::GcHeap *heap = &runtime.heap();
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::script::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return set_abort(result, error);
        }
        if (!key_str) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        ValueHandle rooted_key = runtime.local_value();
        if (!rooted_key) {
            return set_abort(result, oom_error());
        }
        *rooted_key = make_heap_string_value(key_str);
        auto *obj = mutable_heap_ptr<fiber::script::GcObject>(parent);
        if (!obj) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        if (!runtime.run_with_gc_retry(fiber::script::gc_estimate_object_growth_bytes(obj, obj->size + 1),
                                       [&]() { return fiber::script::gc_object_set(heap, obj, key_str, *value); })) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *value);
    }
    return set_exception(result, fiber::script::ExceptionKind::TypeError);
}

CallResult Access::index_set1(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                              ConstValueHandle value, ResultPayload &result) noexcept {
    ResultPayload tmp;
    CallResult status = index_set(runtime, parent, key, value, tmp);
    if (status != CallResult::Success) {
        result = tmp;
        return status;
    }
    return set_value(result, parent ? *parent : fiber::script::JsValue::make_undefined());
}

CallResult Access::prop_get(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle key,
                            ResultPayload &result) noexcept {
    if (!parent) {
        return set_undefined(result);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Object) {
        ScriptAbortReason error = ScriptAbortReason::None;
        fiber::script::GcString *key_str = ensure_heap_string(runtime, key, error);
        if (!key_str && error != ScriptAbortReason::None) {
            return set_abort(result, error);
        }
        if (!key_str) {
            return set_undefined(result);
        }
        auto *obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*parent);
        const fiber::script::JsValue *found = obj ? fiber::script::gc_object_get(obj, key_str) : nullptr;
        return set_value(result, found ? *found : fiber::script::JsValue::make_undefined());
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Array ||
        fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::String) {
        std::size_t len = 0;
        if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Array) {
            auto *arr = fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(*parent);
            len = arr ? arr->size : 0;
        } else if (!string_length(parent, len)) {
            return set_undefined(result);
        }
        return set_value(result, fiber::script::JsValue::make_integer(static_cast<std::int64_t>(len)));
    }
    (void) key;
    return set_undefined(result);
}

CallResult Access::prop_set(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle value,
                            ConstValueHandle key, ResultPayload &result) noexcept {
    if (!parent || fiber::script::js_value_type(*parent) != fiber::script::JsNodeType::Object) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    ScriptRuntime::LocalMark mark(runtime);
    fiber::script::GcHeap *heap = &runtime.heap();
    ScriptAbortReason error = ScriptAbortReason::None;
    fiber::script::GcString *key_str = ensure_heap_string(runtime, key, error);
    if (!key_str && error != ScriptAbortReason::None) {
        return set_abort(result, error);
    }
    if (!key_str) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    ValueHandle rooted_key = runtime.local_value();
    if (!rooted_key) {
        return set_abort(result, oom_error());
    }
    *rooted_key = make_heap_string_value(key_str);
    auto *obj = mutable_heap_ptr<fiber::script::GcObject>(parent);
    if (!obj) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (!runtime.run_with_gc_retry(fiber::script::gc_estimate_object_growth_bytes(obj, obj->size + 1),
                                   [&]() { return fiber::script::gc_object_set(heap, obj, key_str, *value); })) {
        return set_abort(result, oom_error());
    }
    return set_value(result, *value);
}

CallResult Access::prop_set1(ScriptRuntime &runtime, ConstValueHandle parent, ConstValueHandle value,
                             ConstValueHandle key, ResultPayload &result) noexcept {
    ResultPayload tmp;
    CallResult status = prop_set(runtime, parent, value, key, tmp);
    if (status != CallResult::Success) {
        result = tmp;
        return status;
    }
    return set_value(result, parent ? *parent : fiber::script::JsValue::make_undefined());
}

} // namespace fiber::script::run
