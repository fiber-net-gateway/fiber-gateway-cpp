//
// Created by dear on 2025/12/31.
//

#include "JsValueEncode.h"

#include "JsGc.h"

namespace fiber::json {
namespace {

Generator::Result encode_array(Generator &gen, const GcArray *arr);
Generator::Result encode_object(Generator &gen, const GcObject *obj);

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
        result = gen.string(entry.key);
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
    switch (value.type_) {
        case JsNodeType::Null:
            return gen.null_value();
        case JsNodeType::Boolean:
            return gen.bool_value(value.b);
        case JsNodeType::Integer:
            return gen.integer(value.i);
        case JsNodeType::Float:
            return gen.double_value(value.f);
        case JsNodeType::HeapString: {
            auto *str = reinterpret_cast<const GcString *>(value.gc);
            if (!str) {
                return Generator::Result::InvalidString;
            }
            return gen.string(str);
        }
        case JsNodeType::NativeString:
            return gen.string(value.ns.data, value.ns.len);
        case JsNodeType::Array:
            return encode_array(gen, reinterpret_cast<const GcArray *>(value.gc));
        case JsNodeType::Object:
            return encode_object(gen, reinterpret_cast<const GcObject *>(value.gc));
        case JsNodeType::Exception: {
            auto *exc = reinterpret_cast<const GcException *>(value.gc);
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
                result = gen.string(exc->name);
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
                result = gen.string(exc->message);
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
            if (exc->meta.type_ == JsNodeType::Undefined) {
                result = gen.null_value();
            } else {
                result = encode_js_value(gen, exc->meta);
            }
            if (result != Generator::Result::OK) {
                return result;
            }
            return gen.map_close();
        }
        case JsNodeType::NativeBinary:
            return gen.binary(value.nb.data, value.nb.len);
        case JsNodeType::HeapBinary: {
            auto *bin = reinterpret_cast<const GcBinary *>(value.gc);
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
