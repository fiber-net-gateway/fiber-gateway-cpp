#ifndef FIBER_SCRIPT_STD_NODE_TEXT_H
#define FIBER_SCRIPT_STD_NODE_TEXT_H

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

#include "../JsValue.h"
#include "../gc/GcInternal.h"

namespace fiber::script::std_lib {

// Mirrors Jackson's JsonNode.asText() (no-arg): scalars render to their textual
// form (null renders to "null"), while undefined/containers/binaries render to
// "". Note this is the asText() variant, not asText(""): null -> "null" rather
// than "". Used by RandFuncs.canary and HashFuncs.crc32 (Java's asText(null)
// yields the same byte stream since both collapse Binary to empty). Internal
// helper; not a registered script function.
inline void node_as_text(const JsValue &value, std::string &out) {
    switch (js_value_type(value)) {
        case JsNodeType::String: {
            if (js_value_is_borrowed_string(value)) {
                NativeStr native = js_value_native_string(value);
                if (native.len > 0 && native.data != nullptr) {
                    out.append(native.data, native.len);
                }
                return;
            }
            const GcString *str = js_value_heap_ptr<const GcString>(value);
            if (str != nullptr) {
                std::string_view view;
                if (gc_string_utf8_view(str, view) && !view.empty()) {
                    out.append(view);
                }
            }
            return;
        }
        case JsNodeType::Integer: {
            char buffer[32];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), js_value_int64(value));
            if (converted.ec == std::errc{}) {
                out.append(buffer, static_cast<std::size_t>(converted.ptr - buffer));
            }
            return;
        }
        case JsNodeType::Float: {
            double number = js_value_double(value);
            if (std::isnan(number)) {
                out.append("NaN", 3);
                return;
            }
            if (std::isinf(number)) {
                out.append(number < 0 ? "-Infinity" : "Infinity", number < 0 ? 9 : 8);
                return;
            }
            char buffer[64];
            auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number);
            if (converted.ec == std::errc{}) {
                out.append(buffer, static_cast<std::size_t>(converted.ptr - buffer));
            }
            return;
        }
        case JsNodeType::Boolean: {
            bool b = js_value_bool(value);
            out.append(b ? "true" : "false", b ? 4 : 5);
            return;
        }
        case JsNodeType::Null:
            out.append("null", 4);
            return;
        case JsNodeType::Undefined:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
        case JsNodeType::Binary:
        default:
            return;
    }
}

// Mirrors Java JsonUtil.toString(JsonNode) (used by UrlFunc.buildQuery's value
// rendering): identical to node_as_text for scalars and null, but overrides the
// container/binary cases - Array -> "<ArrayNode>", Object -> "<ObjectNode>", Binary
// -> the raw bytes (Java `new String(binaryValue())`, charset-dependent and treated
// as best-effort for non-UTF-8 content). Internal helper; not a registered function.
inline void node_json_to_string(const JsValue &value, std::string &out) {
    switch (js_value_type(value)) {
        case JsNodeType::Array:
            out.append("<ArrayNode>", 11);
            return;
        case JsNodeType::Object:
            out.append("<ObjectNode>", 12);
            return;
        case JsNodeType::Binary: {
            if (js_value_is_borrowed_binary(value)) {
                NativeBin native = js_value_native_binary(value);
                if (native.len > 0 && native.data != nullptr) {
                    out.append(reinterpret_cast<const char *>(native.data), native.len);
                }
                return;
            }
            const GcBinary *bin = js_value_heap_ptr<const GcBinary>(value);
            if (bin != nullptr && bin->len > 0) {
                out.append(reinterpret_cast<const char *>(bin->data), bin->len);
            }
            return;
        }
        default:
            // String/Integer/Float/Boolean/Null (and Undefined/Iterator/Exception -> "")
            // are exactly node_as_text's rendering.
            node_as_text(value, out);
            return;
    }
}

// Returns a borrowed UTF-8 view of a String value. Heap strings containing
// isolated UTF-16 surrogates have no strict UTF-8 view and are rejected.
inline bool string_utf8_view(const JsValue &value, std::string_view &out) noexcept {
    out = {};
    if (js_value_type(value) != JsNodeType::String) {
        return false;
    }
    if (js_value_is_borrowed_string(value)) {
        NativeStr native = js_value_native_string(value);
        if (native.len > 0 && !native.data) {
            return false;
        }
        out = native.len > 0 ? std::string_view(native.data, native.len) : std::string_view{};
        return true;
    }
    const GcString *str = js_value_heap_ptr<const GcString>(value);
    if (str == nullptr) {
        return false;
    }
    return gc_string_utf8_view(str, out);
}

// Raw bytes of a Binary value (borrowed via NativeBin, heap via GcBinary).
// Returns false when value is not a binary.
inline bool binary_bytes(const JsValue &value, const std::uint8_t *&data, std::size_t &len) noexcept {
    if (js_value_type(value) != JsNodeType::Binary) {
        return false;
    }
    if (js_value_is_borrowed_binary(value)) {
        NativeBin native = js_value_native_binary(value);
        data = native.data;
        len = native.len;
        return true;
    }
    const GcBinary *bin = js_value_heap_ptr<const GcBinary>(value);
    if (bin == nullptr) {
        data = nullptr;
        len = 0;
        return false;
    }
    data = bin->data;
    len = bin->len;
    return true;
}

// Lowercase hex of [bytes, bytes+len) into buf (2 chars/byte). buf must hold len*2 chars.
inline void hex_encode(const std::uint8_t *bytes, std::size_t len, char *buf) noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < len; ++i) {
        buf[i * 2] = kHex[(bytes[i] >> 4) & 0x0F];
        buf[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
}

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_NODE_TEXT_H
