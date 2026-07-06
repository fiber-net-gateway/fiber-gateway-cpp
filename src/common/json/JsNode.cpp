//
// Created by dear on 2025/12/30.
//

#include "JsNode.h"

#include "JsGc.h"

#include <bit>
#include <limits>

namespace fiber::json {

namespace {

constexpr std::uint8_t to_u8(JsTag tag) { return static_cast<std::uint8_t>(tag); }

constexpr std::uint8_t to_u8(JsHeapKind kind) { return static_cast<std::uint8_t>(kind); }

constexpr std::uint8_t to_u8(JsBorrowedEncoding encoding) { return static_cast<std::uint8_t>(encoding); }

std::uint32_t clamp_len(std::size_t len) {
    if (len > std::numeric_limits<std::uint32_t>::max()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(len);
}

JsValue make_heap_value(GcHeader *hdr, JsHeapKind kind) {
    JsValue result;
    result.tag = to_u8(JsTag::HeapRef);
    result.subtag = to_u8(kind);
    result.payload = reinterpret_cast<std::uint64_t>(hdr);
    return result;
}

} // namespace

JsValue JsValue::make_undefined() { return JsValue(); }

JsValue JsValue::make_null() {
    JsValue value;
    value.tag = to_u8(JsTag::Null);
    return value;
}

JsValue JsValue::make_boolean(bool value) {
    JsValue result;
    result.tag = to_u8(JsTag::Boolean);
    result.payload = value ? 1u : 0u;
    return result;
}

JsValue JsValue::make_integer(int64_t value) {
    JsValue result;
    result.tag = to_u8(JsTag::Int64);
    result.payload = std::bit_cast<std::uint64_t>(value);
    return result;
}

JsValue JsValue::make_float(double value) {
    JsValue result;
    result.tag = to_u8(JsTag::Double);
    result.payload = std::bit_cast<std::uint64_t>(value);
    return result;
}

JsValue JsValue::make_native_string(const char *data, std::size_t len) {
    return js_make_borrowed_string(data, len, JsBorrowedEncoding::Utf8);
}

JsValue JsValue::make_native_binary(const std::uint8_t *data, std::size_t len) {
    return js_make_borrowed_binary(data, len);
}

JsValue JsValue::make_string(GcHeap &heap, const char *data, std::size_t len) {
    GcString *str = gc_new_string(&heap, data, len);
    if (!str) {
        return make_undefined();
    }
    return make_heap_value(&str->hdr, JsHeapKind::String);
}

JsValue JsValue::make_binary(GcHeap &heap, const std::uint8_t *data, std::size_t len) {
    GcBinary *bin = gc_new_binary(&heap, data, len);
    if (!bin) {
        return make_undefined();
    }
    return make_heap_value(&bin->hdr, JsHeapKind::Binary);
}

JsValue JsValue::make_array(GcHeap &heap, std::size_t capacity) {
    GcArray *arr = gc_new_array(&heap, capacity);
    if (!arr) {
        return make_undefined();
    }
    return make_heap_value(&arr->hdr, JsHeapKind::Array);
}

JsValue JsValue::make_object(GcHeap &heap, std::size_t capacity) {
    GcObject *obj = gc_new_object(&heap, capacity);
    if (!obj) {
        return make_undefined();
    }
    return make_heap_value(&obj->hdr, JsHeapKind::Object);
}

JsValue JsValue::make_exception(ExceptionKind kind) {
    JsValue result;
    result.tag = to_u8(JsTag::Exception);
    result.subtag = static_cast<std::uint8_t>(kind);
    return result;
}

JsValue js_make_heap_ref(GcHeader *hdr, JsHeapKind kind) { return make_heap_value(hdr, kind); }

JsValue js_make_borrowed_string(const char *data, std::size_t len, JsBorrowedEncoding encoding) {
    JsValue result;
    result.tag = to_u8(JsTag::BorrowedString);
    result.subtag = to_u8(encoding);
    result.payload = reinterpret_cast<std::uint64_t>(data);
    result.aux32 = clamp_len(len);
    return result;
}

JsValue js_make_borrowed_binary(const std::uint8_t *data, std::size_t len) {
    JsValue result;
    result.tag = to_u8(JsTag::BorrowedBinary);
    result.payload = reinterpret_cast<std::uint64_t>(data);
    result.aux32 = clamp_len(len);
    return result;
}

JsTag js_value_tag(const JsValue &value) { return static_cast<JsTag>(value.tag); }

JsNodeType js_value_type(const JsValue &value) {
    switch (js_value_tag(value)) {
        case JsTag::Undefined:
            return JsNodeType::Undefined;
        case JsTag::Null:
            return JsNodeType::Null;
        case JsTag::Boolean:
            return JsNodeType::Boolean;
        case JsTag::Int64:
            return JsNodeType::Integer;
        case JsTag::Double:
            return JsNodeType::Float;
        case JsTag::BorrowedString:
            return JsNodeType::String;
        case JsTag::BorrowedBinary:
            return JsNodeType::Binary;
        case JsTag::Exception:
            return JsNodeType::Exception;
        case JsTag::HeapRef:
            switch (static_cast<JsHeapKind>(value.subtag)) {
                case JsHeapKind::String:
                    return JsNodeType::String;
                case JsHeapKind::Binary:
                    return JsNodeType::Binary;
                case JsHeapKind::Array:
                    return JsNodeType::Array;
                case JsHeapKind::Object:
                    return JsNodeType::Object;
                case JsHeapKind::Exception:
                    return JsNodeType::Exception;
                case JsHeapKind::Iterator:
                    return JsNodeType::Interator;
            }
            break;
    }
    return JsNodeType::Undefined;
}

ExceptionKind js_value_exception_kind(const JsValue &value) { return static_cast<ExceptionKind>(value.subtag); }

bool js_value_is_string(const JsValue &value) { return js_value_type(value) == JsNodeType::String; }

bool js_value_is_binary(const JsValue &value) { return js_value_type(value) == JsNodeType::Binary; }

bool js_value_is_heap_ref(const JsValue &value) { return js_value_tag(value) == JsTag::HeapRef && value.payload != 0; }

bool js_value_is_borrowed_string(const JsValue &value) { return js_value_tag(value) == JsTag::BorrowedString; }

bool js_value_is_borrowed_binary(const JsValue &value) { return js_value_tag(value) == JsTag::BorrowedBinary; }

bool js_value_is_undefined(const JsValue &value) { return js_value_tag(value) == JsTag::Undefined; }

bool js_value_bool(const JsValue &value) { return value.payload != 0; }

std::int64_t js_value_int64(const JsValue &value) { return std::bit_cast<std::int64_t>(value.payload); }

double js_value_double(const JsValue &value) { return std::bit_cast<double>(value.payload); }

NativeStr js_value_native_string(const JsValue &value) {
    return NativeStr{
            .len = value.aux32,
            .data = reinterpret_cast<const char *>(value.payload),
    };
}

NativeBin js_value_native_binary(const JsValue &value) {
    return NativeBin{
            .len = value.aux32,
            .data = reinterpret_cast<const std::uint8_t *>(value.payload),
    };
}

GcHeader *js_value_heap_header(JsValue &value) {
    if (!js_value_is_heap_ref(value)) {
        return nullptr;
    }
    return reinterpret_cast<GcHeader *>(value.payload);
}

const GcHeader *js_value_heap_header(const JsValue &value) {
    if (!js_value_is_heap_ref(value)) {
        return nullptr;
    }
    return reinterpret_cast<const GcHeader *>(value.payload);
}

} // namespace fiber::json
