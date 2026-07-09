#include "Access.h"

#include "../../common/json/Utf.h"
#include "../gc/GcInternal.h"

#include <string>

namespace fiber::script::run {

namespace {

ScriptAbortReason oom_error() { return ScriptAbortReason::OutOfMemory; }

fiber::script::JsValue &mutable_value(ConstValueHandle value) noexcept {
    return *const_cast<fiber::script::JsValue *>(value.get());
}

ValueHandle mutable_handle(ConstValueHandle value) noexcept {
    return ValueHandle(value ? const_cast<fiber::script::JsValue *>(value.get()) : nullptr);
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

CallResult string_char_at(GcHeap &runtime, ResultPayload &result, ConstValueHandle value, std::int64_t index) {
    if (index < 0) {
        return set_undefined(result);
    }
    GcHeap::LocalMark mark(runtime);
    ValueHandle rooted_str = runtime.local_value();
    ValueHandle char_value = runtime.local_value();
    if (!rooted_str || !char_value) {
        return set_abort(result, oom_error());
    }
    if (value && fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
        !fiber::script::js_value_is_borrowed_string(*value)) {
        *rooted_str = *value;
    } else if (value && fiber::script::js_value_type(*value) == fiber::script::JsNodeType::String &&
               fiber::script::js_value_is_borrowed_string(*value)) {
        fiber::script::NativeStr native = fiber::script::js_value_native_string(*value);
        if (!fiber::script::gc_make_string(&runtime.heap(), rooted_str, native.data, native.len)) {
            return set_abort(result, oom_error());
        }
    }
    auto *str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(*rooted_str);
    if (!str) {
        return set_abort(result, oom_error());
    }
    if (index >= static_cast<std::int64_t>(str->len)) {
        return set_undefined(result);
    }
    if (str->encoding == fiber::script::GcStringEncoding::Byte) {
        std::uint8_t byte = str->data8[index];
        if (!fiber::script::gc_make_string_bytes(&runtime.heap(), char_value, &byte, 1)) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *char_value);
    }
    char16_t unit = str->data16[index];
    if (!fiber::script::gc_make_string_utf16(&runtime.heap(), char_value, &unit, 1)) {
        return set_abort(result, oom_error());
    }
    return set_value(result, *char_value);
}

} // namespace

CallResult Access::expand_object(GcHeap &runtime, ConstValueHandle target, ConstValueHandle addition,
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
    {
        GcHeap::NoGcScope no_gc(*heap);
        std::size_t expected = target_obj->size + add_obj->size;
        if (!fiber::script::gc_object_reserve(heap, target_obj, expected)) {
            return set_abort(result, oom_error());
        }
        for (const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_first_entry(add_obj);
             entry != nullptr; entry = fiber::script::gc_object_next_entry(add_obj, entry)) {
            if (!entry->occupied || !entry->key) {
                continue;
            }
            if (!fiber::script::gc_object_set_heap_key(heap, mutable_handle(target), entry->key, entry->value)) {
                return set_abort(result, oom_error());
            }
        }
    }
    return set_value(result, *target);
}

CallResult Access::expand_array(GcHeap &runtime, ConstValueHandle target, ConstValueHandle addition,
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
        {
            GcHeap::NoGcScope no_gc(*heap);
            std::size_t expected = target_arr->size + add_arr->size;
            if (!fiber::script::gc_array_reserve(heap, target_arr, expected)) {
                return set_abort(result, oom_error());
            }
            for (std::size_t i = 0; i < add_arr->size; ++i) {
                const fiber::script::JsValue *item = fiber::script::gc_array_get(add_arr, i);
                if (!fiber::script::gc_array_push(heap, mutable_handle(target),
                                                  item ? *item : fiber::script::JsValue::make_undefined())) {
                    return set_abort(result, oom_error());
                }
            }
        }
        return set_value(result, *target);
    }
    auto *add_obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*addition);
    if (!add_obj) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    {
        GcHeap::NoGcScope no_gc(*heap);
        std::size_t expected = target_arr->size + add_obj->size;
        if (!fiber::script::gc_array_reserve(heap, target_arr, expected)) {
            return set_abort(result, oom_error());
        }
        for (const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_first_entry(add_obj);
             entry != nullptr; entry = fiber::script::gc_object_next_entry(add_obj, entry)) {
            if (!entry->occupied) {
                continue;
            }
            if (!fiber::script::gc_array_push(heap, mutable_handle(target), entry->value)) {
                return set_abort(result, oom_error());
            }
        }
    }
    return set_value(result, *target);
}

CallResult Access::push_array(GcHeap &runtime, ConstValueHandle target, ConstValueHandle addition,
                              ResultPayload &result) noexcept {
    if (!target || fiber::script::js_value_type(*target) != fiber::script::JsNodeType::Array) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    fiber::script::GcHeap *heap = &runtime.heap();
    if (!fiber::script::gc_array_push(heap, mutable_handle(target),
                                      addition ? *addition : fiber::script::JsValue::make_undefined())) {
        return set_abort(result, oom_error());
    }
    return set_value(result, *target);
}

CallResult Access::index_get(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key,
                             ResultPayload &result) noexcept {
    if (!parent) {
        return set_undefined(result);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx) || idx < 0) {
            return set_undefined(result);
        }
        GcHeap::LocalMark mark(runtime);
        ValueHandle found = runtime.local_value();
        if (!found) {
            return set_abort(result, oom_error());
        }
        if (!fiber::script::gc_array_get(parent, static_cast<std::size_t>(idx), found)) {
            return set_undefined(result);
        }
        return set_value(result, *found);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Object) {
        if (!key || fiber::script::js_value_type(*key) != fiber::script::JsNodeType::String) {
            return set_undefined(result);
        }
        GcHeap::LocalMark mark(runtime);
        ValueHandle found = runtime.local_value();
        if (!found) {
            return set_abort(result, oom_error());
        }
        if (!fiber::script::gc_object_get(&runtime.heap(), parent, *key, found)) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *found);
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

CallResult Access::index_set(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key, ConstValueHandle value,
                             ResultPayload &result) noexcept {
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
        if (!fiber::script::gc_array_set(heap, mutable_handle(parent), static_cast<std::size_t>(idx), *value)) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *value);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Object) {
        fiber::script::GcHeap *heap = &runtime.heap();
        if (!key || fiber::script::js_value_type(*key) != fiber::script::JsNodeType::String) {
            return set_exception(result, fiber::script::ExceptionKind::TypeError);
        }
        if (!fiber::script::gc_object_set(heap, mutable_handle(parent), *key, *value)) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *value);
    }
    return set_exception(result, fiber::script::ExceptionKind::TypeError);
}

CallResult Access::index_set1(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key, ConstValueHandle value,
                              ResultPayload &result) noexcept {
    ResultPayload tmp;
    CallResult status = index_set(runtime, parent, key, value, tmp);
    if (status != CallResult::Success) {
        result = tmp;
        return status;
    }
    return set_value(result, parent ? *parent : fiber::script::JsValue::make_undefined());
}

CallResult Access::prop_get(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle key,
                            ResultPayload &result) noexcept {
    if (!parent) {
        return set_undefined(result);
    }
    if (fiber::script::js_value_type(*parent) == fiber::script::JsNodeType::Object) {
        if (!key || fiber::script::js_value_type(*key) != fiber::script::JsNodeType::String) {
            return set_undefined(result);
        }
        GcHeap::LocalMark mark(runtime);
        ValueHandle found = runtime.local_value();
        if (!found) {
            return set_abort(result, oom_error());
        }
        if (!fiber::script::gc_object_get(&runtime.heap(), parent, *key, found)) {
            return set_abort(result, oom_error());
        }
        return set_value(result, *found);
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

CallResult Access::prop_set(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle value, ConstValueHandle key,
                            ResultPayload &result) noexcept {
    if (!parent || fiber::script::js_value_type(*parent) != fiber::script::JsNodeType::Object) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    fiber::script::GcHeap *heap = &runtime.heap();
    if (!key || fiber::script::js_value_type(*key) != fiber::script::JsNodeType::String) {
        return set_exception(result, fiber::script::ExceptionKind::TypeError);
    }
    if (!fiber::script::gc_object_set(heap, mutable_handle(parent), *key, *value)) {
        return set_abort(result, oom_error());
    }
    return set_value(result, *value);
}

CallResult Access::prop_set1(GcHeap &runtime, ConstValueHandle parent, ConstValueHandle value, ConstValueHandle key,
                             ResultPayload &result) noexcept {
    ResultPayload tmp;
    CallResult status = prop_set(runtime, parent, value, key, tmp);
    if (status != CallResult::Success) {
        result = tmp;
        return status;
    }
    return set_value(result, parent ? *parent : fiber::script::JsValue::make_undefined());
}

} // namespace fiber::script::run
