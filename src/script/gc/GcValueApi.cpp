//
// Created by dear on 2025/12/30.
//

#include "GcInternal.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace fiber::script {
namespace {

GcString *heap_string_from_value(GcHeap *heap, const JsValue &value, ValueHandle root) noexcept {
    if (!heap || js_value_type(value) != JsNodeType::String) {
        return nullptr;
    }
    if (!js_value_is_borrowed_string(value)) {
        auto *str = const_cast<GcString *>(js_value_heap_ptr<const GcString>(value));
        if (str && root) {
            *root = value;
        }
        return str;
    }

    NativeStr native = js_value_native_string(value);
    GcString *str = nullptr;
    auto encoding = static_cast<JsBorrowedEncoding>(value.subtag);
    if (encoding == JsBorrowedEncoding::Byte) {
        str = gc_new_string_bytes(heap, reinterpret_cast<const std::uint8_t *>(native.data), native.len);
    } else {
        str = gc_new_string(heap, native.data, native.len);
    }
    if (str && root) {
        *root = js_make_heap_ref(&str->hdr, GcHeapKind::String);
    }
    return str;
}

GcArray *mutable_array_from_value(JsValue &value) noexcept {
    if (js_value_type(value) != JsNodeType::Array) {
        return nullptr;
    }
    return js_value_heap_ptr<GcArray>(value);
}

const GcArray *array_from_value(const JsValue &value) noexcept {
    if (js_value_type(value) != JsNodeType::Array) {
        return nullptr;
    }
    return js_value_heap_ptr<const GcArray>(value);
}

GcObject *mutable_object_from_value(JsValue &value) noexcept {
    if (js_value_type(value) != JsNodeType::Object) {
        return nullptr;
    }
    return js_value_heap_ptr<GcObject>(value);
}

const GcObject *object_from_value(const JsValue &value) noexcept {
    if (js_value_type(value) != JsNodeType::Object) {
        return nullptr;
    }
    return js_value_heap_ptr<const GcObject>(value);
}

GcIterator *mutable_iterator_from_value(JsValue &value) noexcept {
    if (js_value_type(value) != JsNodeType::Interator) {
        return nullptr;
    }
    return js_value_heap_ptr<GcIterator>(value);
}

void set_undefined(ValueHandle out) noexcept {
    if (out) {
        *out = JsValue::make_undefined();
    }
}

bool root_inputs(GcHeap *heap, JsValue target, JsValue value, ValueHandle &target_root, ValueHandle &value_root) {
    target_root = heap->local_value();
    value_root = heap->local_value();
    if (!target_root || !value_root) {
        return false;
    }
    *target_root = target;
    *value_root = value;
    return true;
}

} // namespace

bool gc_make_string(GcHeap *heap, ValueHandle out, const char *data, std::size_t len) noexcept {
    if (!heap || !out || (len > 0 && !data)) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcString *str = gc_new_string(heap, data, len);
    if (!str) {
        return false;
    }
    *out = js_make_heap_ref(&str->hdr, GcHeapKind::String);
    return true;
}

bool gc_make_string_bytes(GcHeap *heap, ValueHandle out, const std::uint8_t *data, std::size_t len) noexcept {
    if (!heap || !out || (len > 0 && !data)) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcString *str = gc_new_string_bytes(heap, data, len);
    if (!str) {
        return false;
    }
    *out = js_make_heap_ref(&str->hdr, GcHeapKind::String);
    return true;
}

bool gc_make_string_bytes_uninit(GcHeap *heap, ValueHandle out, std::size_t len, GcByteStringWriter writer,
                                 void *ctx) noexcept {
    if (!heap || !out || !writer) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcString *str = gc_new_string_bytes_uninit(heap, len);
    if (!str) {
        return false;
    }
    if (!writer(str->data8, str->len, ctx)) {
        return false;
    }
    *out = js_make_heap_ref(&str->hdr, GcHeapKind::String);
    return true;
}

bool gc_make_string_utf16(GcHeap *heap, ValueHandle out, const char16_t *data, std::size_t len) noexcept {
    if (!heap || !out || (len > 0 && !data)) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcString *str = gc_new_string_utf16(heap, data, len);
    if (!str) {
        return false;
    }
    *out = js_make_heap_ref(&str->hdr, GcHeapKind::String);
    return true;
}

bool gc_make_string_utf16_uninit(GcHeap *heap, ValueHandle out, std::size_t len, GcUtf16StringWriter writer,
                                 void *ctx) noexcept {
    if (!heap || !out || !writer) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcString *str = gc_new_string_utf16_uninit(heap, len);
    if (!str) {
        return false;
    }
    if (!writer(str->data16, str->len, ctx)) {
        return false;
    }
    *out = js_make_heap_ref(&str->hdr, GcHeapKind::String);
    return true;
}

bool gc_make_binary(GcHeap *heap, ValueHandle out, const std::uint8_t *data, std::size_t len) {
    if (!heap || !out || (len > 0 && !data)) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcBinary *bin = gc_new_binary(heap, data, len);
    if (!bin) {
        return false;
    }
    *out = js_make_heap_ref(&bin->hdr, GcHeapKind::Binary);
    return true;
}

bool gc_make_array(GcHeap *heap, ValueHandle out, std::size_t capacity) {
    if (!heap || !out) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcArray *arr = gc_new_array(heap, capacity);
    if (!arr) {
        return false;
    }
    *out = js_make_heap_ref(&arr->hdr, GcHeapKind::Array);
    return true;
}

bool gc_make_object(GcHeap *heap, ValueHandle out, std::size_t capacity) {
    if (!heap || !out) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcObject *obj = gc_new_object(heap, capacity);
    if (!obj) {
        return false;
    }
    *out = js_make_heap_ref(&obj->hdr, GcHeapKind::Object);
    return true;
}

bool gc_make_exception(GcHeap *heap, ValueHandle out, std::int64_t position, const char *name, std::size_t name_len,
                       const char *message, std::size_t message_len, JsValue meta) {
    if (!heap || !out) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::LocalMark mark(*heap);
    ValueHandle meta_root = heap->local_value();
    if (!meta_root) {
        return false;
    }
    *meta_root = meta;
    GcHeap::NoGcScope no_gc(*heap);
    GcException *exc = gc_new_exception(heap, position, name, name_len, message, message_len, *meta_root);
    if (!exc) {
        return false;
    }
    *out = js_make_heap_ref(&exc->hdr, GcHeapKind::Exception);
    return true;
}

bool gc_make_exception(GcHeap *heap, ValueHandle out, std::int64_t position, const char *name, std::size_t name_len,
                       const char *message, std::size_t message_len) {
    return gc_make_exception(heap, out, position, name, name_len, message, message_len, JsValue::make_undefined());
}

bool gc_make_empty_iterator(GcHeap *heap, ValueHandle out, GcIteratorMode mode) {
    if (!heap || !out) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::NoGcScope no_gc(*heap);
    GcIterator *iter = gc_new_array_iterator(heap, nullptr, mode);
    if (!iter) {
        return false;
    }
    *out = js_make_heap_ref(&iter->hdr, GcHeapKind::Iterator);
    return true;
}

bool gc_make_array_iterator(GcHeap *heap, ValueHandle out, ConstValueHandle array, GcIteratorMode mode) {
    if (!heap || !out || !array || js_value_type(*array) != JsNodeType::Array) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::LocalMark mark(*heap);
    ValueHandle array_root = heap->local_value();
    if (!array_root) {
        return false;
    }
    *array_root = *array;
    GcHeap::NoGcScope no_gc(*heap);
    GcArray *arr = mutable_array_from_value(*array_root);
    if (!arr) {
        return false;
    }
    GcIterator *iter = gc_new_array_iterator(heap, arr, mode);
    if (!iter) {
        return false;
    }
    *out = js_make_heap_ref(&iter->hdr, GcHeapKind::Iterator);
    return true;
}

bool gc_make_object_iterator(GcHeap *heap, ValueHandle out, ConstValueHandle object, GcIteratorMode mode) {
    if (!heap || !out || !object || js_value_type(*object) != JsNodeType::Object) {
        return false;
    }
    *out = JsValue::make_undefined();
    GcHeap::LocalMark mark(*heap);
    ValueHandle object_root = heap->local_value();
    if (!object_root) {
        return false;
    }
    *object_root = *object;
    GcHeap::NoGcScope no_gc(*heap);
    GcObject *obj = mutable_object_from_value(*object_root);
    if (!obj) {
        return false;
    }
    GcIterator *iter = gc_new_object_iterator(heap, obj, mode);
    if (!iter) {
        return false;
    }
    *out = js_make_heap_ref(&iter->hdr, GcHeapKind::Iterator);
    return true;
}

bool gc_string_to_utf8(ConstValueHandle value, std::string &out) {
    out.clear();
    if (!value || js_value_type(*value) != JsNodeType::String || js_value_is_borrowed_string(*value)) {
        return false;
    }
    return gc_string_to_utf8(js_value_heap_ptr<const GcString>(*value), out);
}

bool gc_array_get(ConstValueHandle array, std::size_t index, ValueHandle out) {
    if (!array || !out) {
        return false;
    }
    JsValue target = *array;
    const GcArray *arr = array_from_value(target);
    if (!arr) {
        return false;
    }
    const JsValue *found = gc_array_get(arr, index);
    *out = found ? *found : JsValue::make_undefined();
    return true;
}

bool gc_array_set(GcHeap *heap, ValueHandle array, std::size_t index, JsValue value) {
    if (!heap || !array) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle array_root = nullptr;
    ValueHandle value_root = nullptr;
    if (!root_inputs(heap, *array, value, array_root, value_root)) {
        return false;
    }
    GcHeap::NoGcScope no_gc(*heap);
    GcArray *arr = mutable_array_from_value(*array_root);
    return arr && gc_array_set(heap, arr, index, *value_root);
}

bool gc_array_push(GcHeap *heap, ValueHandle array, JsValue value) {
    if (!heap || !array) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle array_root = nullptr;
    ValueHandle value_root = nullptr;
    if (!root_inputs(heap, *array, value, array_root, value_root)) {
        return false;
    }
    GcHeap::NoGcScope no_gc(*heap);
    GcArray *arr = mutable_array_from_value(*array_root);
    return arr && gc_array_push(heap, arr, *value_root);
}

bool gc_array_pop(ValueHandle array, ValueHandle out) {
    if (!array || !out) {
        return false;
    }
    JsValue target = *array;
    GcArray *arr = mutable_array_from_value(target);
    if (!arr) {
        return false;
    }
    JsValue popped = JsValue::make_undefined();
    bool ok = gc_array_pop(arr, &popped);
    *out = std::move(popped);
    return ok;
}

bool gc_array_insert(GcHeap *heap, ValueHandle array, std::size_t index, JsValue value) {
    if (!heap || !array) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle array_root = nullptr;
    ValueHandle value_root = nullptr;
    if (!root_inputs(heap, *array, value, array_root, value_root)) {
        return false;
    }
    GcHeap::NoGcScope no_gc(*heap);
    GcArray *arr = mutable_array_from_value(*array_root);
    return arr && gc_array_insert(heap, arr, index, *value_root);
}

bool gc_array_remove(ValueHandle array, std::size_t index, ValueHandle out) {
    if (!array || !out) {
        return false;
    }
    JsValue target = *array;
    GcArray *arr = mutable_array_from_value(target);
    if (!arr) {
        return false;
    }
    JsValue removed = JsValue::make_undefined();
    bool ok = gc_array_remove(arr, index, &removed);
    *out = std::move(removed);
    return ok;
}

bool gc_object_set(GcHeap *heap, ValueHandle object, JsValue key, JsValue value) {
    if (!heap || !object) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle object_root = heap->local_value();
    ValueHandle key_root = heap->local_value();
    ValueHandle value_root = heap->local_value();
    if (!object_root || !key_root || !value_root) {
        return false;
    }
    *object_root = *object;
    *key_root = key;
    *value_root = value;

    GcHeap::NoGcScope no_gc(*heap);
    GcObject *obj = mutable_object_from_value(*object_root);
    GcString *key_str = heap_string_from_value(heap, *key_root, key_root);
    return obj && key_str && gc_object_set(heap, obj, key_str, *value_root);
}

bool gc_object_set_key(GcHeap *heap, ValueHandle object, const char *key, std::size_t key_len, JsValue value) {
    if (key_len > 0 && !key) {
        return false;
    }
    return gc_object_set(heap, object, JsValue::make_native_string(key, key_len), value);
}

bool gc_object_set_heap_key(GcHeap *heap, ValueHandle object, const GcString *key, JsValue value) {
    if (!heap || !object || !key) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle object_root = heap->local_value();
    ValueHandle value_root = heap->local_value();
    if (!object_root || !value_root) {
        return false;
    }
    *object_root = *object;
    *value_root = value;

    GcHeap::NoGcScope no_gc(*heap);
    GcObject *obj = mutable_object_from_value(*object_root);
    return obj && gc_object_set(heap, obj, const_cast<GcString *>(key), *value_root);
}

bool gc_object_get(GcHeap *heap, ConstValueHandle object, JsValue key, ValueHandle out) {
    if (!heap || !object || !out) {
        return false;
    }
    JsValue target = *object;
    GcHeap::LocalMark mark(*heap);
    ValueHandle object_root = heap->local_value();
    ValueHandle key_root = heap->local_value();
    if (!object_root || !key_root) {
        return false;
    }
    *object_root = target;
    *key_root = key;
    set_undefined(out);

    GcHeap::NoGcScope no_gc(*heap);
    const GcObject *obj = object_from_value(*object_root);
    GcString *key_str = heap_string_from_value(heap, *key_root, key_root);
    if (!obj || !key_str) {
        return false;
    }
    const JsValue *found = gc_object_get(obj, key_str);
    *out = found ? *found : JsValue::make_undefined();
    return true;
}

bool gc_object_get_key(GcHeap *heap, ConstValueHandle object, const char *key, std::size_t key_len, ValueHandle out) {
    if (key_len > 0 && !key) {
        return false;
    }
    return gc_object_get(heap, object, JsValue::make_native_string(key, key_len), out);
}

bool gc_object_remove(GcHeap *heap, ValueHandle object, JsValue key) {
    if (!heap || !object) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle object_root = heap->local_value();
    ValueHandle key_root = heap->local_value();
    if (!object_root || !key_root) {
        return false;
    }
    *object_root = *object;
    *key_root = key;

    GcHeap::NoGcScope no_gc(*heap);
    GcObject *obj = mutable_object_from_value(*object_root);
    GcString *key_str = heap_string_from_value(heap, *key_root, key_root);
    return obj && key_str && gc_object_remove(obj, key_str);
}

bool gc_object_remove_key(GcHeap *heap, ValueHandle object, const char *key, std::size_t key_len) {
    if (key_len > 0 && !key) {
        return false;
    }
    return gc_object_remove(heap, object, JsValue::make_native_string(key, key_len));
}

bool gc_iterator_next(GcHeap *heap, ValueHandle iter, ValueHandle out, bool &done) {
    done = true;
    if (!heap || !iter || !out) {
        return false;
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle iter_root = heap->local_value();
    if (!iter_root) {
        return false;
    }
    *iter_root = *iter;
    set_undefined(out);

    GcHeap::NoGcScope no_gc(*heap);
    GcIterator *raw_iter = mutable_iterator_from_value(*iter_root);
    if (!raw_iter) {
        return false;
    }
    JsValue next = JsValue::make_undefined();
    if (!gc_iterator_next(heap, raw_iter, next, done)) {
        return false;
    }
    *out = next;
    return true;
}

} // namespace fiber::script
