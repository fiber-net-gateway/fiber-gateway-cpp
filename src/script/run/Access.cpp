#include "Access.h"

#include "../../common/json/Utf.h"

#include <string>

namespace fiber::script::run {

namespace {

VmError make_error(std::string name, std::string message) {
    VmError error;
    error.name = std::move(name);
    error.message = std::move(message);
    return error;
}

VmError heap_required_error() {
    return make_error("EXEC_HEAP_REQUIRED", "heap required for access operation");
}

VmError oom_error() {
    return make_error("EXEC_OUT_OF_MEMORY", "out of memory");
}

VmError index_error(std::string message) {
    return make_error("EXEC_INDEX_ERROR", std::move(message));
}

bool get_index(const fiber::json::JsValue &key, std::int64_t &out) {
    if (key.type_ == fiber::json::JsNodeType::Integer) {
        out = key.i;
        return true;
    }
    return false;
}

fiber::json::GcString *ensure_heap_string(fiber::json::GcHeap *heap,
                                          const fiber::json::JsValue &value,
                                          VmError &error) {
    if (value.type_ == fiber::json::JsNodeType::HeapString) {
        return reinterpret_cast<fiber::json::GcString *>(value.gc);
    }
    if (value.type_ != fiber::json::JsNodeType::NativeString) {
        return nullptr;
    }
    if (!heap) {
        error = heap_required_error();
        return nullptr;
    }
    fiber::json::GcString *str = fiber::json::gc_new_string(heap, value.ns.data, value.ns.len);
    if (!str) {
        error = oom_error();
        return nullptr;
    }
    return str;
}

VmResult make_heap_string_value(fiber::json::GcString *str) {
    fiber::json::JsValue value;
    if (!str) {
        return value;
    }
    value.type_ = fiber::json::JsNodeType::HeapString;
    value.gc = &str->hdr;
    return value;
}

bool string_length(const fiber::json::JsValue &value, std::size_t &out, VmError &error) {
    if (value.type_ == fiber::json::JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const fiber::json::GcString *>(value.gc);
        out = str ? str->len : 0;
        return true;
    }
    if (value.type_ == fiber::json::JsNodeType::NativeString) {
        fiber::json::Utf8ScanResult scan;
        if (!fiber::json::utf8_scan(value.ns.data, value.ns.len, scan)) {
            error = make_error("EXEC_INVALID_UTF8", "invalid utf-8");
            return false;
        }
        out = scan.utf16_len;
        return true;
    }
    return false;
}

VmResult string_char_at(fiber::json::GcHeap *heap,
                        const fiber::json::JsValue &value,
                        std::int64_t index) {
    if (index < 0) {
        return fiber::json::JsValue::make_undefined();
    }
    if (!heap) {
        return std::unexpected(heap_required_error());
    }
    fiber::json::GcString *str = nullptr;
    VmError error;
    if (value.type_ == fiber::json::JsNodeType::HeapString) {
        str = reinterpret_cast<fiber::json::GcString *>(value.gc);
    } else if (value.type_ == fiber::json::JsNodeType::NativeString) {
        str = fiber::json::gc_new_string(heap, value.ns.data, value.ns.len);
    }
    if (!str) {
        return std::unexpected(oom_error());
    }
    if (index >= static_cast<std::int64_t>(str->len)) {
        return fiber::json::JsValue::make_undefined();
    }
    if (str->encoding == fiber::json::GcStringEncoding::Byte) {
        std::uint8_t byte = str->data8[index];
        fiber::json::GcString *out =
            fiber::json::gc_new_string_bytes(heap, &byte, 1);
        if (!out) {
            return std::unexpected(oom_error());
        }
        return make_heap_string_value(out);
    }
    char16_t unit = str->data16[index];
    fiber::json::GcString *out = fiber::json::gc_new_string_utf16(heap, &unit, 1);
    if (!out) {
        return std::unexpected(oom_error());
    }
    return make_heap_string_value(out);
}

} // namespace

VmResult Access::expand_object(const fiber::json::JsValue &target,
                               const fiber::json::JsValue &addition,
                               fiber::json::GcHeap *heap) {
    if (target.type_ != fiber::json::JsNodeType::Object ||
        addition.type_ != fiber::json::JsNodeType::Object) {
        return target;
    }
    if (!heap) {
        return std::unexpected(heap_required_error());
    }
    auto *target_obj = reinterpret_cast<fiber::json::GcObject *>(target.gc);
    auto *add_obj = reinterpret_cast<const fiber::json::GcObject *>(addition.gc);
    if (!target_obj || !add_obj) {
        return target;
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied || !entry->key) {
            continue;
        }
        if (!fiber::json::gc_object_set(heap, target_obj, entry->key, entry->value)) {
            return std::unexpected(oom_error());
        }
    }
    return target;
}

VmResult Access::expand_array(const fiber::json::JsValue &target,
                              const fiber::json::JsValue &addition,
                              fiber::json::GcHeap *heap) {
    if (target.type_ != fiber::json::JsNodeType::Array) {
        return target;
    }
    if (addition.type_ != fiber::json::JsNodeType::Array &&
        addition.type_ != fiber::json::JsNodeType::Object) {
        return target;
    }
    if (!heap) {
        return std::unexpected(heap_required_error());
    }
    auto *target_arr = reinterpret_cast<fiber::json::GcArray *>(target.gc);
    if (!target_arr) {
        return target;
    }
    if (addition.type_ == fiber::json::JsNodeType::Array) {
        auto *add_arr = reinterpret_cast<const fiber::json::GcArray *>(addition.gc);
        if (!add_arr) {
            return target;
        }
        for (std::size_t i = 0; i < add_arr->size; ++i) {
            const fiber::json::JsValue *item = fiber::json::gc_array_get(add_arr, i);
            if (!fiber::json::gc_array_push(heap, target_arr, item ? *item : fiber::json::JsValue::make_undefined())) {
                return std::unexpected(oom_error());
            }
        }
        return target;
    }
    auto *add_obj = reinterpret_cast<const fiber::json::GcObject *>(addition.gc);
    if (!add_obj) {
        return target;
    }
    for (std::size_t i = 0; i < add_obj->size; ++i) {
        const fiber::json::GcObjectEntry *entry = fiber::json::gc_object_entry_at(add_obj, i);
        if (!entry || !entry->occupied) {
            continue;
        }
        if (!fiber::json::gc_array_push(heap, target_arr, entry->value)) {
            return std::unexpected(oom_error());
        }
    }
    return target;
}

VmResult Access::push_array(const fiber::json::JsValue &target,
                            const fiber::json::JsValue &addition,
                            fiber::json::GcHeap *heap) {
    if (target.type_ != fiber::json::JsNodeType::Array) {
        return target;
    }
    if (!heap) {
        return std::unexpected(heap_required_error());
    }
    auto *arr = reinterpret_cast<fiber::json::GcArray *>(target.gc);
    if (!arr) {
        return target;
    }
    if (!fiber::json::gc_array_push(heap, arr, addition)) {
        return std::unexpected(oom_error());
    }
    return target;
}

VmResult Access::index_get(const fiber::json::JsValue &parent,
                           const fiber::json::JsValue &key,
                           fiber::json::GcHeap *heap) {
    if (parent.type_ == fiber::json::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return fiber::json::JsValue::make_undefined();
        }
        if (idx < 0) {
            return fiber::json::JsValue::make_undefined();
        }
        auto *arr = reinterpret_cast<const fiber::json::GcArray *>(parent.gc);
        const fiber::json::JsValue *found = arr ? fiber::json::gc_array_get(arr, static_cast<std::size_t>(idx)) : nullptr;
        return found ? *found : fiber::json::JsValue::make_undefined();
    }
    if (parent.type_ == fiber::json::JsNodeType::Object) {
        VmError error;
        fiber::json::GcString *key_str = ensure_heap_string(heap, key, error);
        if (!key_str && error.name.size()) {
            return std::unexpected(error);
        }
        if (!key_str) {
            return fiber::json::JsValue::make_undefined();
        }
        auto *obj = reinterpret_cast<const fiber::json::GcObject *>(parent.gc);
        const fiber::json::JsValue *found = obj ? fiber::json::gc_object_get(obj, key_str) : nullptr;
        return found ? *found : fiber::json::JsValue::make_undefined();
    }
    if (parent.type_ == fiber::json::JsNodeType::HeapString ||
        parent.type_ == fiber::json::JsNodeType::NativeString) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return fiber::json::JsValue::make_undefined();
        }
        return string_char_at(heap, parent, idx);
    }
    return fiber::json::JsValue::make_undefined();
}

VmResult Access::index_set(const fiber::json::JsValue &parent,
                           const fiber::json::JsValue &key,
                           const fiber::json::JsValue &value,
                           fiber::json::GcHeap *heap) {
    if (parent.type_ == fiber::json::JsNodeType::Array) {
        std::int64_t idx = 0;
        if (!get_index(key, idx)) {
            return std::unexpected(index_error("array index must be integer"));
        }
        auto *arr = reinterpret_cast<fiber::json::GcArray *>(parent.gc);
        if (!arr || idx < 0 || idx >= static_cast<std::int64_t>(arr->size)) {
            return std::unexpected(index_error("array index out of bounds"));
        }
        if (!heap) {
            return std::unexpected(heap_required_error());
        }
        if (!fiber::json::gc_array_set(heap, arr, static_cast<std::size_t>(idx), value)) {
            return std::unexpected(oom_error());
        }
        return value;
    }
    if (parent.type_ == fiber::json::JsNodeType::Object) {
        VmError error;
        fiber::json::GcString *key_str = ensure_heap_string(heap, key, error);
        if (!key_str && error.name.size()) {
            return std::unexpected(error);
        }
        if (!key_str) {
            return std::unexpected(index_error("object key must be string"));
        }
        auto *obj = reinterpret_cast<fiber::json::GcObject *>(parent.gc);
        if (!heap) {
            return std::unexpected(heap_required_error());
        }
        if (!fiber::json::gc_object_set(heap, obj, key_str, value)) {
            return std::unexpected(oom_error());
        }
        return value;
    }
    return std::unexpected(index_error("indexing not supported"));
}

VmResult Access::index_set1(const fiber::json::JsValue &parent,
                            const fiber::json::JsValue &key,
                            const fiber::json::JsValue &value,
                            fiber::json::GcHeap *heap) {
    VmResult result = index_set(parent, key, value, heap);
    if (!result) {
        return result;
    }
    return parent;
}

VmResult Access::prop_get(const fiber::json::JsValue &parent,
                          const fiber::json::JsValue &key,
                          fiber::json::GcHeap *heap) {
    if (parent.type_ == fiber::json::JsNodeType::Object) {
        VmError error;
        fiber::json::GcString *key_str = ensure_heap_string(heap, key, error);
        if (!key_str && error.name.size()) {
            return std::unexpected(error);
        }
        if (!key_str) {
            return fiber::json::JsValue::make_undefined();
        }
        auto *obj = reinterpret_cast<const fiber::json::GcObject *>(parent.gc);
        const fiber::json::JsValue *found = obj ? fiber::json::gc_object_get(obj, key_str) : nullptr;
        return found ? *found : fiber::json::JsValue::make_undefined();
    }
    if (parent.type_ == fiber::json::JsNodeType::Array ||
        parent.type_ == fiber::json::JsNodeType::HeapString ||
        parent.type_ == fiber::json::JsNodeType::NativeString) {
        std::size_t len = 0;
        VmError error;
        if (parent.type_ == fiber::json::JsNodeType::Array) {
            auto *arr = reinterpret_cast<const fiber::json::GcArray *>(parent.gc);
            len = arr ? arr->size : 0;
        } else if (!string_length(parent, len, error)) {
            return std::unexpected(error);
        }
        return fiber::json::JsValue::make_integer(static_cast<std::int64_t>(len));
    }
    (void)key;
    return fiber::json::JsValue::make_undefined();
}

VmResult Access::prop_set(const fiber::json::JsValue &parent,
                          const fiber::json::JsValue &value,
                          const fiber::json::JsValue &key,
                          fiber::json::GcHeap *heap) {
    if (parent.type_ != fiber::json::JsNodeType::Object) {
        return std::unexpected(index_error("property set not supported"));
    }
    VmError error;
    fiber::json::GcString *key_str = ensure_heap_string(heap, key, error);
    if (!key_str && error.name.size()) {
        return std::unexpected(error);
    }
    if (!key_str) {
        return std::unexpected(index_error("property key must be string"));
    }
    auto *obj = reinterpret_cast<fiber::json::GcObject *>(parent.gc);
    if (!heap) {
        return std::unexpected(heap_required_error());
    }
    if (!fiber::json::gc_object_set(heap, obj, key_str, value)) {
        return std::unexpected(oom_error());
    }
    return value;
}

VmResult Access::prop_set1(const fiber::json::JsValue &parent,
                           const fiber::json::JsValue &value,
                           const fiber::json::JsValue &key,
                           fiber::json::GcHeap *heap) {
    VmResult result = prop_set(parent, value, key, heap);
    if (!result) {
        return result;
    }
    return parent;
}

} // namespace fiber::script::run
