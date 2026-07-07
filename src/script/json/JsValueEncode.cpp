//
// Created by dear on 2025/12/31.
//

#include "JsValueEncode.h"

#include "JsGc.h"

#include <string>

namespace fiber::json {
namespace {

Generator::Result encode_array(Generator &gen, const GcArray *arr);
Generator::Result encode_object(Generator &gen, const GcObject *obj);

Generator::Result encode_gc_string(Generator &gen, const GcString *str) {
    if (!str) {
        return Generator::Result::InvalidString;
    }
    std::string utf8;
    if (!gc_string_to_utf8(str, utf8)) {
        return Generator::Result::InvalidString;
    }
    return gen.string(utf8);
}

Generator::Result encode_array(Generator &gen, const GcArray *arr) {
    if (!arr) {
        return Generator::Result::InvalidValue;
    }
    Generator::Result result = gen.array_open();
    if (result != Generator::Result::OK) {
        return result;
    }
    for (std::size_t i = 0; i < arr->size; ++i) {
        result = encode_js_value(gen, arr->elems[i]);
        if (result != Generator::Result::OK) {
            return result;
        }
    }
    return gen.array_close();
}

Generator::Result encode_object(Generator &gen, const GcObject *obj) {
    if (!obj) {
        return Generator::Result::InvalidValue;
    }
    Generator::Result result = gen.map_open();
    if (result != Generator::Result::OK) {
        return result;
    }
    int32_t cursor = obj->head;
    while (cursor != -1) {
        const GcObjectEntry &entry = obj->entries[cursor];
        if (!entry.occupied || !entry.key) {
            return Generator::Result::InvalidValue;
        }
        result = encode_gc_string(gen, entry.key);
        if (result != Generator::Result::OK) {
            return result;
        }
        result = encode_js_value(gen, entry.value);
        if (result != Generator::Result::OK) {
            return result;
        }
        cursor = entry.next_order;
    }
    return gen.map_close();
}

} // namespace

Generator::Result encode_js_value(Generator &gen, const JsValue &value) {
    switch (js_value_type(value)) {
        case JsNodeType::Null:
            return gen.null_value();
        case JsNodeType::Boolean:
            return gen.bool_value(js_value_bool(value));
        case JsNodeType::Integer:
            return gen.integer(js_value_int64(value));
        case JsNodeType::Float:
            return gen.double_value(js_value_double(value));
        case JsNodeType::String:
            if (js_value_is_borrowed_string(value)) {
                NativeStr native = js_value_native_string(value);
                return gen.string(native.data, native.len);
            } else {
                auto *str = js_value_heap_ptr<const GcString>(value);
                if (!str) {
                    return Generator::Result::InvalidString;
                }
                return encode_gc_string(gen, str);
            }
        case JsNodeType::Array:
            return encode_array(gen, js_value_heap_ptr<const GcArray>(value));
        case JsNodeType::Object:
            return encode_object(gen, js_value_heap_ptr<const GcObject>(value));
        case JsNodeType::Exception: {
            auto *exc = js_value_heap_ptr<const GcException>(value);
            if (!exc) {
                return Generator::Result::InvalidValue;
            }
            Generator::Result result = gen.map_open();
            if (result != Generator::Result::OK) {
                return result;
            }
            result = gen.string("position", 8);
            if (result != Generator::Result::OK) {
                return result;
            }
            result = gen.integer(exc->position);
            if (result != Generator::Result::OK) {
                return result;
            }
            result = gen.string("name", 4);
            if (result != Generator::Result::OK) {
                return result;
            }
            if (exc->name) {
                result = encode_gc_string(gen, exc->name);
            } else {
                result = gen.null_value();
            }
            if (result != Generator::Result::OK) {
                return result;
            }
            result = gen.string("message", 7);
            if (result != Generator::Result::OK) {
                return result;
            }
            if (exc->message) {
                result = encode_gc_string(gen, exc->message);
            } else {
                result = gen.null_value();
            }
            if (result != Generator::Result::OK) {
                return result;
            }
            result = gen.string("meta", 4);
            if (result != Generator::Result::OK) {
                return result;
            }
            if (js_value_is_undefined(exc->meta)) {
                result = gen.null_value();
            } else {
                result = encode_js_value(gen, exc->meta);
            }
            if (result != Generator::Result::OK) {
                return result;
            }
            return gen.map_close();
        }
        case JsNodeType::Binary:
            if (js_value_is_borrowed_binary(value)) {
                NativeBin native = js_value_native_binary(value);
                return gen.binary(native.data, native.len);
            } else {
                auto *bin = js_value_heap_ptr<const GcBinary>(value);
                if (!bin) {
                    return Generator::Result::InvalidString;
                }
                return gen.binary(bin->data, bin->len);
            }
        case JsNodeType::Undefined:
        case JsNodeType::Interator:
            return Generator::Result::InvalidValue;
    }
    return Generator::Result::InvalidValue;
}

} // namespace fiber::json
