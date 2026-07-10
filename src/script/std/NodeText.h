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

} // namespace fiber::script::std_lib

#endif // FIBER_SCRIPT_STD_NODE_TEXT_H
