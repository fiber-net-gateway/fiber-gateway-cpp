//
// Created by dear on 2025/12/30.
//

#include "JsNode.h"

#include "JsGc.h"

#include <memory>
#include <utility>

namespace fiber::json {

JsValue JsValue::make_undefined() {
    return JsValue();
}

JsValue JsValue::make_null() {
    JsValue value;
    value.type_ = JsNodeType::Null;
    value.i = 0;
    return value;
}

JsValue JsValue::make_integer(int64_t value) {
    JsValue result;
    result.type_ = JsNodeType::Integer;
    result.i = value;
    return result;
}

JsValue JsValue::make_float(double value) {
    JsValue result;
    result.type_ = JsNodeType::Float;
    result.f = value;
    return result;
}

JsValue JsValue::make_native_string(char *data, std::size_t len) {
    JsValue result;
    result.type_ = JsNodeType::NativeString;
    result.ns.len = len;
    result.ns.data = data;
    return result;
}

JsValue JsValue::make_native_binary(std::uint8_t *data, std::size_t len) {
    JsValue result;
    result.type_ = JsNodeType::NativeBinary;
    result.nb.len = len;
    result.nb.data = data;
    return result;
}

JsValue JsValue::make_string(GcHeap &heap, const char *data, std::size_t len) {
    JsValue result;
    GcString *str = gc_new_string(&heap, data, len);
    if (!str) {
        return result;
    }
    result.type_ = JsNodeType::HeapString;
    result.gc = &str->hdr;
    return result;
}

JsValue JsValue::make_binary(GcHeap &heap, const std::uint8_t *data, std::size_t len) {
    JsValue result;
    GcBinary *bin = gc_new_binary(&heap, data, len);
    if (!bin) {
        return result;
    }
    result.type_ = JsNodeType::HeapBinary;
    result.gc = &bin->hdr;
    return result;
}

JsValue JsValue::make_array(GcHeap &heap, std::size_t capacity) {
    JsValue result;
    GcArray *arr = gc_new_array(&heap, capacity);
    if (!arr) {
        return result;
    }
    result.type_ = JsNodeType::Array;
    result.gc = &arr->hdr;
    return result;
}

JsValue JsValue::make_object(GcHeap &heap, std::size_t capacity) {
    JsValue result;
    GcObject *obj = gc_new_object(&heap, capacity);
    if (!obj) {
        return result;
    }
    result.type_ = JsNodeType::Object;
    result.gc = &obj->hdr;
    return result;
}

JsValue::JsValue()
    : type_(JsNodeType::Undefined), i(0) {}

JsValue::JsValue(const JsValue &other)
    : type_(JsNodeType::Undefined), i(0) {
    copy_from(other);
}

JsValue::JsValue(JsValue &&other) noexcept
    : type_(JsNodeType::Undefined), i(0) {
    move_from(std::move(other));
}

JsValue &JsValue::operator=(const JsValue &other) {
    if (this != &other) {
        destroy();
        copy_from(other);
    }
    return *this;
}

JsValue &JsValue::operator=(JsValue &&other) noexcept {
    if (this != &other) {
        destroy();
        move_from(std::move(other));
    }
    return *this;
}

JsValue::~JsValue() {
    destroy();
}

void JsValue::destroy() {
    switch (type_) {
        case JsNodeType::Integer:
            std::destroy_at(&i);
            break;
        case JsNodeType::Float:
            std::destroy_at(&f);
            break;
        case JsNodeType::NativeString:
            std::destroy_at(&ns);
            break;
        case JsNodeType::NativeBinary:
            std::destroy_at(&nb);
            break;
        case JsNodeType::HeapString:
        case JsNodeType::HeapBinary:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
            std::destroy_at(&gc);
            break;
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            break;
    }
    type_ = JsNodeType::Undefined;
    i = 0;
}

void JsValue::copy_from(const JsValue &other) {
    type_ = other.type_;
    switch (other.type_) {
        case JsNodeType::Integer:
            std::construct_at(&i, other.i);
            break;
        case JsNodeType::Float:
            std::construct_at(&f, other.f);
            break;
        case JsNodeType::NativeString:
            std::construct_at(&ns, other.ns);
            break;
        case JsNodeType::NativeBinary:
            std::construct_at(&nb, other.nb);
            break;
        case JsNodeType::HeapString:
        case JsNodeType::HeapBinary:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
            std::construct_at(&gc, other.gc);
            break;
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            std::construct_at(&i, int64_t{0});
            break;
    }
}

void JsValue::move_from(JsValue &&other) {
    type_ = other.type_;
    switch (other.type_) {
        case JsNodeType::Integer:
            std::construct_at(&i, other.i);
            break;
        case JsNodeType::Float:
            std::construct_at(&f, other.f);
            break;
        case JsNodeType::NativeString:
            std::construct_at(&ns, std::move(other.ns));
            break;
        case JsNodeType::NativeBinary:
            std::construct_at(&nb, std::move(other.nb));
            break;
        case JsNodeType::HeapString:
        case JsNodeType::HeapBinary:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
            std::construct_at(&gc, other.gc);
            break;
        case JsNodeType::Undefined:
        case JsNodeType::Null:
            std::construct_at(&i, int64_t{0});
            break;
    }
    other.type_ = JsNodeType::Undefined;
    other.i = 0;
}

} // namespace fiber::json
