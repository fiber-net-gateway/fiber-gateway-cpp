#include "StdLibrary.h"

#include "../../common/json/JsGc.h"
#include "../../common/json/JsValueOps.h"
#include "../../common/json/JsonDecode.h"
#include "../../common/json/JsonEncode.h"
#include "../../common/json/JsValueEncode.h"
#include "../../common/json/Utf.h"
#include "../Runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fiber::script::std_lib {
namespace {

using fiber::json::GcArray;
using fiber::json::GcBinary;
using fiber::json::GcObject;
using fiber::json::GcObjectEntry;
using fiber::json::GcString;
using fiber::json::JsNodeType;
using fiber::json::JsValue;
using FunctionResult = Library::FunctionResult;

constexpr std::string_view kNullText = "null";
constexpr std::string_view kNilText = "<nil>";
constexpr std::string_view kArrayText = "<ArrayNode>";
constexpr std::string_view kObjectText = "<ObjectNode>";

std::string_view type_name(JsNodeType type) {
    switch (type) {
        case JsNodeType::Undefined:
            return "Undefined";
        case JsNodeType::Null:
            return "Null";
        case JsNodeType::Boolean:
            return "Boolean";
        case JsNodeType::Integer:
            return "Integer";
        case JsNodeType::Float:
            return "Float";
        case JsNodeType::HeapString:
        case JsNodeType::NativeString:
            return "String";
        case JsNodeType::Array:
            return "Array";
        case JsNodeType::Object:
            return "Object";
        case JsNodeType::Interator:
            return "Iterator";
        case JsNodeType::Exception:
            return "Exception";
        case JsNodeType::NativeBinary:
        case JsNodeType::HeapBinary:
            return "Binary";
    }
    return "Unknown";
}

bool is_string_type(const JsValue &value) {
    return value.type_ == JsNodeType::HeapString || value.type_ == JsNodeType::NativeString;
}

bool is_binary_type(const JsValue &value) {
    return value.type_ == JsNodeType::HeapBinary || value.type_ == JsNodeType::NativeBinary;
}

bool is_number_type(const JsValue &value) {
    return value.type_ == JsNodeType::Integer || value.type_ == JsNodeType::Float;
}

bool get_utf8_string(const JsValue &value, std::string &out) {
    out.clear();
    if (value.type_ == JsNodeType::NativeString) {
        if (value.ns.len == 0) {
            return true;
        }
        if (!value.ns.data) {
            return false;
        }
        if (!fiber::json::utf8_validate(value.ns.data, value.ns.len)) {
            return false;
        }
        out.assign(value.ns.data, value.ns.len);
        return true;
    }
    if (value.type_ == JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const GcString *>(value.gc);
        return fiber::json::gc_string_to_utf8(str, out);
    }
    return false;
}

bool get_u16_string(const JsValue &value, std::u16string &out) {
    out.clear();
    if (value.type_ == JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const GcString *>(value.gc);
        if (!str) {
            return false;
        }
        if (str->encoding == fiber::json::GcStringEncoding::Byte) {
            out.resize(str->len);
            for (std::size_t i = 0; i < str->len; ++i) {
                out[i] = static_cast<char16_t>(str->data8[i]);
            }
        } else {
            out.assign(str->data16, str->data16 + str->len);
        }
        return true;
    }
    if (value.type_ == JsNodeType::NativeString) {
        if (value.ns.len == 0) {
            return true;
        }
        fiber::json::Utf8ScanResult scan;
        if (!fiber::json::utf8_scan(value.ns.data, value.ns.len, scan)) {
            return false;
        }
        out.resize(scan.utf16_len);
        if (!fiber::json::utf8_write_utf16(value.ns.data, value.ns.len, out.data(), scan.utf16_len)) {
            return false;
        }
        return true;
    }
    return false;
}

bool string_length(const JsValue &value, std::size_t &out) {
    out = 0;
    if (value.type_ == JsNodeType::HeapString) {
        auto *str = reinterpret_cast<const GcString *>(value.gc);
        if (!str) {
            return false;
        }
        out = str->len;
        return true;
    }
    if (value.type_ == JsNodeType::NativeString) {
        fiber::json::Utf8ScanResult scan;
        if (!fiber::json::utf8_scan(value.ns.data, value.ns.len, scan)) {
            return false;
        }
        out = scan.utf16_len;
        return true;
    }
    return false;
}

bool get_binary_data(const JsValue &value, const std::uint8_t *&data, std::size_t &len) {
    data = nullptr;
    len = 0;
    if (value.type_ == JsNodeType::NativeBinary) {
        data = value.nb.data;
        len = value.nb.len;
        return true;
    }
    if (value.type_ == JsNodeType::HeapBinary) {
        auto *bin = reinterpret_cast<const GcBinary *>(value.gc);
        if (!bin) {
            return false;
        }
        data = bin->data;
        len = bin->len;
        return true;
    }
    return false;
}

bool to_double(const JsValue &value, double &out) {
    if (value.type_ == JsNodeType::Integer) {
        out = static_cast<double>(value.i);
        return true;
    }
    if (value.type_ == JsNodeType::Float) {
        out = value.f;
        return true;
    }
    return false;
}

bool to_int64(const JsValue &value, std::int64_t &out) {
    if (value.type_ == JsNodeType::Integer) {
        out = value.i;
        return true;
    }
    if (value.type_ == JsNodeType::Float) {
        out = static_cast<std::int64_t>(value.f);
        return true;
    }
    return false;
}

std::int64_t to_int64_default(const JsValue &value, std::int64_t fallback = 0) {
    std::int64_t out = fallback;
    if (to_int64(value, out)) {
        return out;
    }
    return fallback;
}

std::string double_to_string(double value) {
    std::ostringstream oss;
    oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
    oss << std::setprecision(15) << value;
    return oss.str();
}

std::string as_text(const JsValue &value, std::string_view default_value) {
    switch (value.type_) {
        case JsNodeType::HeapString:
        case JsNodeType::NativeString: {
            std::string out;
            if (get_utf8_string(value, out)) {
                return out;
            }
            return std::string(default_value);
        }
        case JsNodeType::Integer:
            return std::to_string(value.i);
        case JsNodeType::Float:
            return double_to_string(value.f);
        case JsNodeType::Boolean:
            return value.b ? "true" : "false";
        case JsNodeType::Null:
        case JsNodeType::Undefined:
        case JsNodeType::Array:
        case JsNodeType::Object:
        case JsNodeType::Interator:
        case JsNodeType::Exception:
        case JsNodeType::NativeBinary:
        case JsNodeType::HeapBinary:
            return std::string(default_value);
    }
    return std::string(default_value);
}

std::string jsonutil_to_string(const JsValue &value) {
    switch (value.type_) {
        case JsNodeType::Undefined:
            return std::string(kNilText);
        case JsNodeType::Null:
            return std::string(kNullText);
        case JsNodeType::Boolean:
            return value.b ? "true" : "false";
        case JsNodeType::Integer:
            return std::to_string(value.i);
        case JsNodeType::Float:
            return double_to_string(value.f);
        case JsNodeType::HeapString:
        case JsNodeType::NativeString: {
            std::string out;
            if (get_utf8_string(value, out)) {
                return out;
            }
            return std::string(kNilText);
        }
        case JsNodeType::Array:
            return std::string(kArrayText);
        case JsNodeType::Object:
        case JsNodeType::Exception:
            return std::string(kObjectText);
        case JsNodeType::Interator:
            return std::string(kArrayText);
        case JsNodeType::NativeBinary:
            return std::string(reinterpret_cast<const char *>(value.nb.data), value.nb.len);
        case JsNodeType::HeapBinary: {
            auto *bin = reinterpret_cast<const GcBinary *>(value.gc);
            if (!bin) {
                return std::string(kNilText);
            }
            return std::string(reinterpret_cast<const char *>(bin->data), bin->len);
        }
    }
    return std::string(kNilText);
}

JsValue make_heap_string_value(ScriptRuntime &runtime, std::string_view text) {
    auto *str = runtime.alloc_with_gc(text.size(), [&]() {
        return fiber::json::gc_new_string(&runtime.heap(), text.data(), text.size());
    });
    if (!str) {
        return JsValue::make_undefined();
    }
    JsValue value;
    value.type_ = JsNodeType::HeapString;
    value.gc = &str->hdr;
    return value;
}

JsValue make_heap_string_value_u16(ScriptRuntime &runtime, const std::u16string &text) {
    bool all_byte = true;
    for (char16_t ch : text) {
        if (ch > 0xFF) {
            all_byte = false;
            break;
        }
    }
    if (all_byte) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(text.size());
        for (char16_t ch : text) {
            bytes.push_back(static_cast<std::uint8_t>(ch));
        }
        auto *str = runtime.alloc_with_gc(bytes.size(), [&]() {
            return fiber::json::gc_new_string_bytes(&runtime.heap(), bytes.data(), bytes.size());
        });
        if (!str) {
            return JsValue::make_undefined();
        }
        JsValue value;
        value.type_ = JsNodeType::HeapString;
        value.gc = &str->hdr;
        return value;
    }
    auto *str = runtime.alloc_with_gc(text.size() * sizeof(char16_t), [&]() {
        return fiber::json::gc_new_string_utf16(&runtime.heap(), text.data(), text.size());
    });
    if (!str) {
        return JsValue::make_undefined();
    }
    JsValue value;
    value.type_ = JsNodeType::HeapString;
    value.gc = &str->hdr;
    return value;
}

JsValue make_heap_binary_value(ScriptRuntime &runtime, const std::uint8_t *data, std::size_t len) {
    auto *bin = runtime.alloc_with_gc(len, [&]() {
        return fiber::json::gc_new_binary(&runtime.heap(), data, len);
    });
    if (!bin) {
        return JsValue::make_undefined();
    }
    JsValue value;
    value.type_ = JsNodeType::HeapBinary;
    value.gc = &bin->hdr;
    return value;
}

Library::FunctionResult make_error(ExecutionContext &context, std::string_view message) {
    JsValue err = make_heap_string_value(context.runtime(), message);
    if (err.type_ == JsNodeType::Undefined) {
        static char fallback[] = "error";
        err = JsValue::make_native_string(fallback, sizeof(fallback) - 1);
    }
    return std::unexpected(err);
}

Library::FunctionResult make_oom_error(ExecutionContext &context) {
    return make_error(context, "out of memory");
}

Library::FunctionResult make_type_error(ExecutionContext &context, std::string_view prefix, const JsValue &value) {
    std::string message(prefix);
    message.append(type_name(value.type_));
    return make_error(context, message);
}

bool is_space(char16_t ch) {
    if (ch > 0x7F) {
        return false;
    }
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

std::u16string trim_left_repeat(const std::u16string &src, const std::u16string &search) {
    if (src.empty() || search.empty()) {
        return src;
    }
    std::size_t pos = 0;
    while (pos + search.size() <= src.size() &&
           src.compare(pos, search.size(), search) == 0) {
        pos += search.size();
    }
    return src.substr(pos);
}

std::u16string trim_right_repeat(const std::u16string &src, const std::u16string &search) {
    if (src.empty() || search.empty()) {
        return src;
    }
    std::size_t len = search.size();
    std::size_t end = src.size();
    while (end >= len && src.compare(end - len, len, search) == 0) {
        end -= len;
    }
    return src.substr(0, end);
}

std::u16string trim_repeat(const std::u16string &src, const std::u16string &search) {
    if (src.empty() || search.empty()) {
        return src;
    }
    std::u16string left = trim_left_repeat(src, search);
    return trim_right_repeat(left, search);
}

std::u16string trim_left_space(const std::u16string &src) {
    std::size_t i = 0;
    while (i < src.size() && is_space(src[i])) {
        ++i;
    }
    if (i >= src.size()) {
        return {};
    }
    return src.substr(i);
}

std::u16string trim_right_space(const std::u16string &src) {
    if (src.empty()) {
        return src;
    }
    std::size_t i = src.size();
    while (i > 0 && is_space(src[i - 1])) {
        --i;
    }
    return src.substr(0, i);
}

std::vector<std::u16string> split_any(const std::u16string &src, const std::u16string &seps) {
    std::vector<std::u16string> out;
    if (src.empty()) {
        return out;
    }
    if (seps.empty()) {
        out.push_back(src);
        return out;
    }
    std::size_t start = 0;
    bool matched = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (seps.find(src[i]) != std::u16string::npos) {
            if (matched) {
                out.push_back(src.substr(start, i - start));
                matched = false;
            }
            start = i + 1;
            continue;
        }
        matched = true;
    }
    if (matched) {
        out.push_back(src.substr(start));
    }
    return out;
}

bool contains_any(const std::u16string &src, const std::u16string &search) {
    if (src.empty() || search.empty()) {
        return false;
    }
    for (char16_t ch : src) {
        if (search.find(ch) != std::u16string::npos) {
            return true;
        }
    }
    return false;
}

int index_of_any(const std::u16string &src, const std::u16string &search) {
    if (src.empty() || search.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (search.find(src[i]) != std::u16string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int last_index_any(const std::u16string &src, const std::u16string &search) {
    if (src.empty() || search.empty()) {
        return -1;
    }
    for (char16_t ch : search) {
        auto pos = src.rfind(ch);
        if (pos != std::u16string::npos) {
            return static_cast<int>(pos);
        }
    }
    return -1;
}

std::string hex_encode(const std::uint8_t *data, std::size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool hex_decode(std::string_view input, std::vector<std::uint8_t> &out) {
    out.clear();
    std::size_t len = input.size() & ~static_cast<std::size_t>(1);
    if (len == 0) {
        return true;
    }
    out.reserve(len / 2);
    for (std::size_t i = 0; i < len; i += 2) {
        int hi = hex_digit(input[i]);
        int lo = hex_digit(input[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string base64_encode(const std::uint8_t *data, std::size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t chunk = data[i] << 16;
        if (i + 1 < len) {
            chunk |= data[i + 1] << 8;
        }
        if (i + 2 < len) {
            chunk |= data[i + 2];
        }
        out.push_back(kTable[(chunk >> 18) & 0x3F]);
        out.push_back(kTable[(chunk >> 12) & 0x3F]);
        if (i + 1 < len) {
            out.push_back(kTable[(chunk >> 6) & 0x3F]);
        } else {
            out.push_back('=');
        }
        if (i + 2 < len) {
            out.push_back(kTable[chunk & 0x3F]);
        } else {
            out.push_back('=');
        }
    }
    return out;
}

bool base64_decode(std::string_view input, std::vector<std::uint8_t> &out) {
    static std::array<int, 256> table = [] {
        std::array<int, 256> t{};
        t.fill(-1);
        for (int i = 'A'; i <= 'Z'; ++i) {
            t[static_cast<std::size_t>(i)] = i - 'A';
        }
        for (int i = 'a'; i <= 'z'; ++i) {
            t[static_cast<std::size_t>(i)] = i - 'a' + 26;
        }
        for (int i = '0'; i <= '9'; ++i) {
            t[static_cast<std::size_t>(i)] = i - '0' + 52;
        }
        t[static_cast<std::size_t>('+')] = 62;
        t[static_cast<std::size_t>('/')] = 63;
        return t;
    }();

    out.clear();
    int val = 0;
    int valb = -8;
    for (unsigned char ch : input) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        int decoded = table[ch];
        if (decoded < 0) {
            return false;
        }
        val = (val << 6) + decoded;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

std::array<std::uint32_t, 256> &crc32_table() {
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                if (c & 1) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c >>= 1;
                }
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data, std::size_t len) {
    auto &table = crc32_table();
    for (std::size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

std::uint32_t crc32_finish(std::uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t crc32(const std::uint8_t *data, std::size_t len) {
    std::uint32_t crc = crc32_update(0xFFFFFFFFu, data, len);
    return crc32_finish(crc);
}

std::uint32_t md5_left_rotate(std::uint32_t x, std::uint32_t c) {
    return (x << c) | (x >> (32 - c));
}

std::array<std::uint8_t, 16> md5_digest(const std::uint8_t *data, std::size_t len) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
        0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
        0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
        0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
        0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
        0xeb86d391u};
    static constexpr std::array<std::uint32_t, 64> s = {
        7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
        14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23, 4,
        11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

    std::array<std::uint32_t, 4> h = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::size_t new_len = len + 1;
    while ((new_len % 64) != 56) {
        ++new_len;
    }
    std::vector<std::uint8_t> buffer(new_len + 8);
    std::memcpy(buffer.data(), data, len);
    buffer[len] = 0x80;
    std::uint64_t bits_len = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) {
        buffer[new_len + i] = static_cast<std::uint8_t>((bits_len >> (8 * i)) & 0xFFu);
    }

    for (std::size_t offset = 0; offset < buffer.size(); offset += 64) {
        std::uint32_t w[16];
        for (int i = 0; i < 16; ++i) {
            std::size_t idx = offset + static_cast<std::size_t>(i) * 4;
            w[i] = static_cast<std::uint32_t>(buffer[idx]) |
                   (static_cast<std::uint32_t>(buffer[idx + 1]) << 8) |
                   (static_cast<std::uint32_t>(buffer[idx + 2]) << 16) |
                   (static_cast<std::uint32_t>(buffer[idx + 3]) << 24);
        }
        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        for (std::uint32_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::uint32_t g = 0;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + md5_left_rotate(a + f + k[i] + w[g], s[i]);
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
    }

    std::array<std::uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(h[i] & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
    }
    return out;
}

std::uint32_t sha1_left_rotate(std::uint32_t x, std::uint32_t n) {
    return (x << n) | (x >> (32 - n));
}

std::array<std::uint8_t, 20> sha1_digest(const std::uint8_t *data, std::size_t len) {
    std::array<std::uint32_t, 5> h = {
        0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::size_t new_len = len + 1;
    while ((new_len % 64) != 56) {
        ++new_len;
    }
    std::vector<std::uint8_t> buffer(new_len + 8);
    std::memcpy(buffer.data(), data, len);
    buffer[len] = 0x80;
    std::uint64_t bits_len = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) {
        buffer[new_len + 7 - i] = static_cast<std::uint8_t>((bits_len >> (8 * i)) & 0xFFu);
    }

    for (std::size_t offset = 0; offset < buffer.size(); offset += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            std::size_t idx = offset + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(buffer[idx]) << 24) |
                   (static_cast<std::uint32_t>(buffer[idx + 1]) << 16) |
                   (static_cast<std::uint32_t>(buffer[idx + 2]) << 8) |
                   static_cast<std::uint32_t>(buffer[idx + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = sha1_left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            std::uint32_t temp = sha1_left_rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = sha1_left_rotate(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<std::uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
    }
    return out;
}

std::uint32_t sha256_rotr(std::uint32_t x, std::uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

std::array<std::uint8_t, 32> sha256_digest(const std::uint8_t *data, std::size_t len) {
    static constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};

    std::array<std::uint32_t, 8> h = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::size_t new_len = len + 1;
    while ((new_len % 64) != 56) {
        ++new_len;
    }
    std::vector<std::uint8_t> buffer(new_len + 8);
    std::memcpy(buffer.data(), data, len);
    buffer[len] = 0x80;
    std::uint64_t bits_len = static_cast<std::uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) {
        buffer[new_len + 7 - i] = static_cast<std::uint8_t>((bits_len >> (8 * i)) & 0xFFu);
    }

    for (std::size_t offset = 0; offset < buffer.size(); offset += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            std::size_t idx = offset + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(buffer[idx]) << 24) |
                   (static_cast<std::uint32_t>(buffer[idx + 1]) << 16) |
                   (static_cast<std::uint32_t>(buffer[idx + 2]) << 8) |
                   static_cast<std::uint32_t>(buffer[idx + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            std::uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            std::uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];
        std::uint32_t f = h[5];
        std::uint32_t g = h[6];
        std::uint32_t hh = h[7];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
            std::uint32_t ch = (e & f) ^ ((~e) & g);
            std::uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
            std::uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
            std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            std::uint32_t temp2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
    }
    return out;
}

bool format_time_pattern(std::string_view pattern, const std::tm &tm, int millis, std::string &out) {
    auto append_number = [&](int value, int width) {
        std::ostringstream oss;
        if (width > 1) {
            oss << std::setw(width) << std::setfill('0');
        }
        oss << value;
        out.append(oss.str());
    };

    for (std::size_t i = 0; i < pattern.size();) {
        char ch = pattern[i];
        if (ch == '\'') {
            std::size_t end = pattern.find('\'', i + 1);
            if (end == std::string_view::npos) {
                return false;
            }
            out.append(pattern.substr(i + 1, end - i - 1));
            i = end + 1;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(ch))) {
            std::size_t j = i + 1;
            while (j < pattern.size() && pattern[j] == ch) {
                ++j;
            }
            std::size_t len = j - i;
            switch (ch) {
                case 'y': {
                    int year = tm.tm_year + 1900;
                    if (len == 2) {
                        append_number(year % 100, 2);
                    } else if (len >= 4) {
                        append_number(year, static_cast<int>(len));
                    } else if (len == 1) {
                        append_number(year, 0);
                    } else {
                        return false;
                    }
                    break;
                }
                case 'M': {
                    int month = tm.tm_mon + 1;
                    append_number(month, static_cast<int>(len));
                    break;
                }
                case 'd': {
                    append_number(tm.tm_mday, static_cast<int>(len));
                    break;
                }
                case 'H': {
                    append_number(tm.tm_hour, static_cast<int>(len));
                    break;
                }
                case 'm': {
                    append_number(tm.tm_min, static_cast<int>(len));
                    break;
                }
                case 's': {
                    append_number(tm.tm_sec, static_cast<int>(len));
                    break;
                }
                case 'S': {
                    int value = millis;
                    if (len < 3) {
                        int mod = 1;
                        for (std::size_t n = 0; n < len; ++n) {
                            mod *= 10;
                        }
                        value = millis % mod;
                    }
                    append_number(value, static_cast<int>(len));
                    break;
                }
                default:
                    return false;
            }
            i = j;
            continue;
        }
        out.push_back(ch);
        ++i;
    }
    return true;
}

std::string format_rfc1123(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::gmtime(&t);
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm) == 0) {
        return {};
    }
    return buf;
}

bool format_time(std::string_view pattern, std::chrono::system_clock::time_point tp, bool local, std::string &out) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = local ? *std::localtime(&t) : *std::gmtime(&t);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    int millis = static_cast<int>(ms % 1000);
    out.clear();
    return format_time_pattern(pattern, tm, millis, out);
}

bool url_encode(std::string_view input, std::string &out) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    out.clear();
    out.reserve(input.size());
    for (unsigned char ch : input) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '*') {
            out.push_back(static_cast<char>(ch));
        } else if (ch == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0xF]);
            out.push_back(kHex[ch & 0xF]);
        }
    }
    return true;
}

bool url_decode(std::string_view input, std::string &out) {
    out.clear();
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        char ch = input[i];
        if (ch == '+') {
            out.push_back(' ');
            continue;
        }
        if (ch == '%') {
            if (i + 2 >= input.size()) {
                return false;
            }
            int hi = hex_digit(input[i + 1]);
            int lo = hex_digit(input[i + 2]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
        }
        out.push_back(ch);
    }
    return true;
}

class LengthFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_integer(0);
        }
        const JsValue &value = context.arg_value(0);
        if (is_string_type(value)) {
            std::size_t len = 0;
            if (!string_length(value, len)) {
                return make_error(context, "invalid utf-8");
            }
            return JsValue::make_integer(static_cast<std::int64_t>(len));
        }
        if (is_binary_type(value)) {
            std::size_t len = 0;
            const std::uint8_t *data = nullptr;
            if (!get_binary_data(value, data, len)) {
                return JsValue::make_integer(0);
            }
            return JsValue::make_integer(static_cast<std::int64_t>(len));
        }
        if (value.type_ == JsNodeType::Array) {
            auto *arr = reinterpret_cast<const GcArray *>(value.gc);
            return JsValue::make_integer(arr ? static_cast<std::int64_t>(arr->size) : 0);
        }
        if (value.type_ == JsNodeType::Object) {
            auto *obj = reinterpret_cast<const GcObject *>(value.gc);
            return JsValue::make_integer(obj ? static_cast<std::int64_t>(obj->size) : 0);
        }
        return JsValue::make_integer(0);
    }
};

class IncludesFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_boolean(false);
        }
        const JsValue &container = context.arg_value(0);
        if (container.type_ == JsNodeType::HeapString || container.type_ == JsNodeType::NativeString) {
            std::string text;
            if (!get_utf8_string(container, text)) {
                return JsValue::make_boolean(false);
            }
            for (std::size_t i = 1; i < context.arg_count(); ++i) {
                const JsValue &arg = context.arg_value(i);
                std::string item;
                if (!get_utf8_string(arg, item)) {
                    return JsValue::make_boolean(false);
                }
                if (text.find(item) == std::string::npos) {
                    return JsValue::make_boolean(false);
                }
            }
            return JsValue::make_boolean(true);
        }
        if (container.type_ != JsNodeType::Array) {
            return JsValue::make_boolean(false);
        }
        auto *arr = reinterpret_cast<const GcArray *>(container.gc);
        if (!arr) {
            return JsValue::make_boolean(false);
        }
        for (std::size_t i = 1; i < context.arg_count(); ++i) {
            const JsValue &arg = context.arg_value(i);
            bool found = false;
            for (std::size_t j = 0; j < arr->size; ++j) {
                const JsValue *elem = fiber::json::gc_array_get(arr, j);
                if (!elem) {
                    continue;
                }
                fiber::json::JsOpResult cmp = fiber::json::js_binary_op(
                    fiber::json::JsBinaryOp::StrictEq, *elem, arg, nullptr);
                if (cmp.error == fiber::json::JsOpError::None && cmp.value.b) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return JsValue::make_boolean(false);
            }
        }
        return JsValue::make_boolean(true);
    }
};

class ArrayJoinFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "array join require array but get none");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Array) {
            return make_type_error(context, "array join require array but get ", arg);
        }
        auto *arr = reinterpret_cast<const GcArray *>(arg.gc);
        std::string delimiter;
        if (context.arg_count() >= 2) {
            delimiter = as_text(context.arg_value(1), "");
        }
        std::string out;
        if (arr) {
            for (std::size_t i = 0; i < arr->size; ++i) {
                if (i > 0) {
                    out.append(delimiter);
                }
                const JsValue *item = fiber::json::gc_array_get(arr, i);
                if (item) {
                    out.append(as_text(*item, ""));
                }
            }
        }
        JsValue result = make_heap_string_value(context.runtime(), out);
        if (result.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return result;
    }
};

class ArrayPopFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "array pop require array but get none");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Array) {
            return make_type_error(context, "array pop require array but get ", arg);
        }
        auto *arr = reinterpret_cast<GcArray *>(arg.gc);
        if (!arr || arr->size == 0) {
            return JsValue::make_null();
        }
        JsValue out = JsValue::make_undefined();
        if (!fiber::json::gc_array_pop(arr, &out)) {
            return JsValue::make_null();
        }
        return out;
    }
};

class ArrayPushFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "array pop require array but get none");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Array) {
            return make_type_error(context, "array pop require array but get ", arg);
        }
        auto *arr = reinterpret_cast<GcArray *>(arg.gc);
        if (!arr) {
            return arg;
        }
        ScriptRuntime &runtime = context.runtime();
        for (std::size_t i = 1; i < context.arg_count(); ++i) {
            if (!fiber::json::gc_array_push(&runtime.heap(), arr, context.arg_value(i))) {
                return make_oom_error(context);
            }
        }
        return arg;
    }
};

GcString *ensure_heap_string(ScriptRuntime &runtime, const JsValue &value) {
    if (value.type_ == JsNodeType::HeapString) {
        return reinterpret_cast<GcString *>(value.gc);
    }
    if (value.type_ == JsNodeType::NativeString) {
        auto *str = runtime.alloc_with_gc(value.ns.len, [&]() {
            return fiber::json::gc_new_string(&runtime.heap(), value.ns.data, value.ns.len);
        });
        return str;
    }
    return nullptr;
}

class ObjectAssignFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "require object");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Object) {
            return make_type_error(context, "require object but get ", arg);
        }
        if (context.arg_count() < 2) {
            return make_error(context, "assignObject empty params");
        }
        auto *target = reinterpret_cast<GcObject *>(arg.gc);
        if (!target) {
            return arg;
        }
        ScriptRuntime &runtime = context.runtime();
        fiber::json::GcHeap *heap = &runtime.heap();
        for (std::size_t i = 1; i < context.arg_count(); ++i) {
            const JsValue &src = context.arg_value(i);
            if (src.type_ != JsNodeType::Object) {
                continue;
            }
            auto *obj = reinterpret_cast<const GcObject *>(src.gc);
            if (!obj) {
                continue;
            }
            for (std::size_t idx = 0; idx < obj->size; ++idx) {
                const GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj, idx);
                if (!entry || !entry->occupied || !entry->key) {
                    continue;
                }
                if (!fiber::json::gc_object_set(heap, target, entry->key, entry->value)) {
                    return make_oom_error(context);
                }
            }
        }
        return arg;
    }
};

class ObjectKeysFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "require object");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Object) {
            return make_type_error(context, "require object but get ", arg);
        }
        ScriptRuntime &runtime = context.runtime();
        auto *obj = reinterpret_cast<const GcObject *>(arg.gc);
        std::size_t capacity = obj ? obj->size : 0;
        JsValue array = JsValue::make_array(runtime.heap(), capacity);
        if (array.type_ != JsNodeType::Array) {
            return make_oom_error(context);
        }
        GcRootGuard guard(runtime, &array);
        auto *arr = reinterpret_cast<GcArray *>(array.gc);
        if (obj && arr) {
            for (std::size_t idx = 0; idx < obj->size; ++idx) {
                const GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj, idx);
                if (!entry || !entry->occupied || !entry->key) {
                    continue;
                }
                JsValue key;
                key.type_ = JsNodeType::HeapString;
                key.gc = &entry->key->hdr;
                if (!fiber::json::gc_array_push(&runtime.heap(), arr, key)) {
                    return make_oom_error(context);
                }
            }
        }
        return array;
    }
};

class ObjectValuesFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "require object");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Object) {
            return make_type_error(context, "require object but get ", arg);
        }
        ScriptRuntime &runtime = context.runtime();
        auto *obj = reinterpret_cast<const GcObject *>(arg.gc);
        std::size_t capacity = obj ? obj->size : 0;
        JsValue array = JsValue::make_array(runtime.heap(), capacity);
        if (array.type_ != JsNodeType::Array) {
            return make_oom_error(context);
        }
        GcRootGuard guard(runtime, &array);
        auto *arr = reinterpret_cast<GcArray *>(array.gc);
        if (obj && arr) {
            for (std::size_t idx = 0; idx < obj->size; ++idx) {
                const GcObjectEntry *entry = fiber::json::gc_object_entry_at(obj, idx);
                if (!entry || !entry->occupied) {
                    continue;
                }
                if (!fiber::json::gc_array_push(&runtime.heap(), arr, entry->value)) {
                    return make_oom_error(context);
                }
            }
        }
        return array;
    }
};

class ObjectDeletePropsFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return make_error(context, "assign ObjectKey params undefined");
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ != JsNodeType::Object) {
            return make_type_error(context, "assign ObjectKey not support ", arg);
        }
        auto *obj = reinterpret_cast<GcObject *>(arg.gc);
        if (!obj) {
            return arg;
        }
        ScriptRuntime &runtime = context.runtime();
        for (std::size_t i = 1; i < context.arg_count(); ++i) {
            const JsValue &key_val = context.arg_value(i);
            if (!is_string_type(key_val)) {
                continue;
            }
            GcString *key = ensure_heap_string(runtime, key_val);
            if (!key) {
                return make_oom_error(context);
            }
            fiber::json::gc_object_remove(obj, key);
        }
        return arg;
    }
};

class HasPrefixFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_boolean(false);
        }
        std::u16string src;
        std::u16string prefix;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_boolean(false);
        }
        if (!get_u16_string(context.arg_value(1), prefix)) {
            return JsValue::make_boolean(false);
        }
        if (prefix.size() > src.size()) {
            return JsValue::make_boolean(false);
        }
        return JsValue::make_boolean(std::equal(prefix.begin(), prefix.end(), src.begin()));
    }
};

class HasSuffixFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_boolean(false);
        }
        std::u16string src;
        std::u16string suffix;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_boolean(false);
        }
        if (!get_u16_string(context.arg_value(1), suffix)) {
            return JsValue::make_boolean(false);
        }
        if (suffix.size() > src.size()) {
            return JsValue::make_boolean(false);
        }
        return JsValue::make_boolean(std::equal(suffix.begin(), suffix.end(), src.end() - suffix.size()));
    }
};

class ToLowerFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        for (auto &ch : src) {
            if (ch <= 0x7F) {
                ch = static_cast<char16_t>(std::tolower(static_cast<unsigned char>(ch)));
            }
        }
        JsValue out = make_heap_string_value_u16(context.runtime(), src);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class ToUpperFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        for (auto &ch : src) {
            if (ch <= 0x7F) {
                ch = static_cast<char16_t>(std::toupper(static_cast<unsigned char>(ch)));
            }
        }
        JsValue out = make_heap_string_value_u16(context.runtime(), src);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class TrimFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (context.arg_count() < 2 || !is_string_type(context.arg_value(1))) {
            std::u16string left = trim_left_space(src);
            std::u16string trimmed = trim_right_space(left);
            JsValue out = make_heap_string_value_u16(context.runtime(), trimmed);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::u16string search;
        if (!get_u16_string(context.arg_value(1), search)) {
            return JsValue::make_null();
        }
        std::u16string trimmed = trim_repeat(src, search);
        JsValue out = make_heap_string_value_u16(context.runtime(), trimmed);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class TrimLeftFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (context.arg_count() < 2 || !is_string_type(context.arg_value(1))) {
            std::u16string trimmed = trim_left_space(src);
            JsValue out = make_heap_string_value_u16(context.runtime(), trimmed);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::u16string search;
        if (!get_u16_string(context.arg_value(1), search)) {
            return JsValue::make_null();
        }
        std::u16string trimmed = trim_left_repeat(src, search);
        JsValue out = make_heap_string_value_u16(context.runtime(), trimmed);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class TrimRightFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (context.arg_count() < 2 || !is_string_type(context.arg_value(1))) {
            std::u16string trimmed = trim_right_space(src);
            JsValue out = make_heap_string_value_u16(context.runtime(), trimmed);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::u16string search;
        if (!get_u16_string(context.arg_value(1), search)) {
            return JsValue::make_null();
        }
        std::u16string trimmed = trim_right_repeat(src, search);
        JsValue out = make_heap_string_value_u16(context.runtime(), trimmed);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class SplitFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        ScriptRuntime &runtime = context.runtime();
        JsValue array = JsValue::make_array(runtime.heap(), 0);
        if (array.type_ != JsNodeType::Array) {
            return make_oom_error(context);
        }
        GcRootGuard guard(runtime, &array);
        auto *arr = reinterpret_cast<GcArray *>(array.gc);
        if (context.arg_count() < 2 || !is_string_type(context.arg_value(1))) {
            JsValue item = make_heap_string_value_u16(runtime, src);
            if (item.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            if (!fiber::json::gc_array_push(&runtime.heap(), arr, item)) {
                return make_oom_error(context);
            }
            return array;
        }
        std::u16string sep;
        if (!get_u16_string(context.arg_value(1), sep)) {
            return JsValue::make_null();
        }
        auto parts = split_any(src, sep);
        for (const auto &part : parts) {
            JsValue item = make_heap_string_value_u16(runtime, part);
            if (item.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            if (!fiber::json::gc_array_push(&runtime.heap(), arr, item)) {
                return make_oom_error(context);
            }
        }
        return array;
    }
};

class FindAllFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::string text;
        std::string pattern;
        if (!get_utf8_string(context.arg_value(0), text)) {
            return JsValue::make_null();
        }
        if (!get_utf8_string(context.arg_value(1), pattern)) {
            return JsValue::make_null();
        }
        std::regex re;
        try {
            re = std::regex(pattern);
        } catch (const std::regex_error &) {
            return make_error(context, "invalid regex");
        }
        ScriptRuntime &runtime = context.runtime();
        JsValue array = JsValue::make_array(runtime.heap(), 0);
        if (array.type_ != JsNodeType::Array) {
            return make_oom_error(context);
        }
        GcRootGuard guard(runtime, &array);
        auto *arr = reinterpret_cast<GcArray *>(array.gc);
        std::string::const_iterator search_start = text.cbegin();
        std::smatch match;
        while (std::regex_search(search_start, text.cend(), match, re)) {
            std::string value = match.str();
            JsValue item = make_heap_string_value(runtime, value);
            if (item.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            if (!fiber::json::gc_array_push(&runtime.heap(), arr, item)) {
                return make_oom_error(context);
            }
            search_start = match.suffix().first;
        }
        return array;
    }
};

class ContainsFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        std::u16string needle;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (!get_u16_string(context.arg_value(1), needle)) {
            return JsValue::make_null();
        }
        bool ok = src.find(needle) != std::u16string::npos;
        return JsValue::make_boolean(ok);
    }
};

class ContainsAnyFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        std::u16string search;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (!get_u16_string(context.arg_value(1), search)) {
            return JsValue::make_null();
        }
        return JsValue::make_boolean(contains_any(src, search));
    }
};

class IndexFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        std::u16string needle;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (!get_u16_string(context.arg_value(1), needle)) {
            return JsValue::make_null();
        }
        if (needle.empty()) {
            return JsValue::make_integer(0);
        }
        auto pos = src.find(needle);
        if (pos == std::u16string::npos) {
            return JsValue::make_integer(-1);
        }
        return JsValue::make_integer(static_cast<std::int64_t>(pos));
    }
};

class IndexAnyFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        std::u16string search;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (!get_u16_string(context.arg_value(1), search)) {
            return JsValue::make_null();
        }
        return JsValue::make_integer(index_of_any(src, search));
    }
};

class LastIndexFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        std::u16string needle;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (!get_u16_string(context.arg_value(1), needle)) {
            return JsValue::make_null();
        }
        if (needle.empty()) {
            return JsValue::make_integer(static_cast<std::int64_t>(src.size()));
        }
        auto pos = src.rfind(needle);
        if (pos == std::u16string::npos) {
            return JsValue::make_integer(-1);
        }
        return JsValue::make_integer(static_cast<std::int64_t>(pos));
    }
};

class LastIndexAnyFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        std::u16string search;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (!get_u16_string(context.arg_value(1), search)) {
            return JsValue::make_null();
        }
        return JsValue::make_integer(last_index_any(src, search));
    }
};

class RepeatFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        const JsValue &count_val = context.arg_value(1);
        if (!is_number_type(count_val)) {
            return JsValue::make_null();
        }
        int count = static_cast<int>(to_int64_default(count_val));
        if (count < 0) {
            return JsValue::make_null();
        }
        if (count == 0 || src.empty()) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        if (count == 1) {
            return context.arg_value(0);
        }
        std::u16string out;
        out.reserve(src.size() * static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            out.append(src);
        }
        JsValue result = make_heap_string_value_u16(context.runtime(), out);
        if (result.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return result;
    }
};

class MatchFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 2) {
            return JsValue::make_boolean(false);
        }
        std::string text;
        std::string pattern;
        if (!get_utf8_string(context.arg_value(0), text)) {
            return JsValue::make_boolean(false);
        }
        if (!get_utf8_string(context.arg_value(1), pattern)) {
            return JsValue::make_boolean(false);
        }
        try {
            std::regex re(pattern);
            return JsValue::make_boolean(std::regex_match(text, re));
        } catch (const std::regex_error &) {
            return make_error(context, "invalid regex");
        }
    }
};

class SubstringFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_null();
        }
        std::u16string src;
        if (!get_u16_string(context.arg_value(0), src)) {
            return JsValue::make_null();
        }
        if (context.arg_count() == 1) {
            return context.arg_value(0);
        }
        std::int64_t i = to_int64_default(context.arg_value(1));
        std::size_t len = src.size();
        if (context.arg_count() == 2) {
            if (i <= 0) {
                return context.arg_value(0);
            }
            if (static_cast<std::size_t>(i) >= len) {
                JsValue out = make_heap_string_value(context.runtime(), "");
                if (out.type_ == JsNodeType::Undefined) {
                    return make_oom_error(context);
                }
                return out;
            }
            std::u16string sub = src.substr(static_cast<std::size_t>(i));
            JsValue out = make_heap_string_value_u16(context.runtime(), sub);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::int64_t j = to_int64_default(context.arg_value(2));
        if (i < 0) {
            i = 0;
        }
        if (static_cast<std::size_t>(i) >= len) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        if (j <= i) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::size_t end = static_cast<std::size_t>(j);
        if (end > len) {
            end = len;
        }
        std::u16string sub = src.substr(static_cast<std::size_t>(i), end - static_cast<std::size_t>(i));
        JsValue out = make_heap_string_value_u16(context.runtime(), sub);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class ToStringFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        const JsValue &arg = context.arg_value(0);
        if (arg.type_ == JsNodeType::Null || arg.type_ == JsNodeType::Undefined) {
            JsValue out = make_heap_string_value(context.runtime(), kNullText);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::string text = jsonutil_to_string(arg);
        JsValue out = make_heap_string_value(context.runtime(), text);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class JsonParseFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "parseJson not support Undefined");
        }
        std::string text;
        if (!get_utf8_string(context.arg_value(0), text)) {
            return make_type_error(context, "parseJson not support ", context.arg_value(0));
        }
        fiber::json::Parser parser(context.runtime().heap());
        JsValue out;
        if (!parser.parse(text, out)) {
            std::string message = "cannot parseJson: ";
            message.append(parser.error().message);
            return make_error(context, message);
        }
        return out;
    }
};

class JsonStringifyFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return make_error(context, "error invoke jsonStringify: empty args");
        }
        class StringSink final : public fiber::json::OutputSink {
        public:
            bool write(const char *data, size_t len) override {
                out.append(data, len);
                return true;
            }
            std::string out;
        };
        StringSink sink;
        fiber::json::Generator gen(sink);
        fiber::json::Generator::Result result = fiber::json::encode_js_value(gen, context.arg_value(0));
        if (result != fiber::json::Generator::Result::OK) {
            return make_error(context, "error invoke jsonStringify: encode failed");
        }
        JsValue out = make_heap_string_value(context.runtime(), sink.out);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class MathFloorFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0 || !is_number_type(context.arg_value(0))) {
            return make_error(context, "require numeric value. and len 1");
        }
        const JsValue &value = context.arg_value(0);
        if (value.type_ == JsNodeType::Integer) {
            return value;
        }
        double v = 0.0;
        if (!to_double(value, v)) {
            return make_error(context, "require numeric value. and len 1");
        }
        double f = std::floor(v);
        return JsValue::make_integer(static_cast<std::int64_t>(f));
    }
};

class MathAbsFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0 || !is_number_type(context.arg_value(0))) {
            return make_error(context, "require numeric value. and len 1");
        }
        const JsValue &value = context.arg_value(0);
        if (value.type_ == JsNodeType::Integer) {
            return JsValue::make_integer(std::llabs(value.i));
        }
        double v = 0.0;
        if (!to_double(value, v)) {
            return make_error(context, "require numeric value. and len 1");
        }
        return JsValue::make_float(std::fabs(v));
    }
};

class BinaryBase64EncodeFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        const JsValue &arg = context.arg_value(0);
        const std::uint8_t *data = nullptr;
        std::size_t len = 0;
        if (!get_binary_data(arg, data, len)) {
            return JsValue::make_undefined();
        }
        std::string encoded = base64_encode(data, len);
        JsValue out = make_heap_string_value(context.runtime(), encoded);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class BinaryBase64DecodeFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        std::string text;
        if (!get_utf8_string(context.arg_value(0), text)) {
            return JsValue::make_undefined();
        }
        std::vector<std::uint8_t> decoded;
        if (!base64_decode(text, decoded)) {
            return make_error(context, "invalid base64");
        }
        JsValue out = make_heap_binary_value(context.runtime(), decoded.data(), decoded.size());
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class BinaryHexFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        const JsValue &arg = context.arg_value(0);
        const std::uint8_t *data = nullptr;
        std::size_t len = 0;
        if (!get_binary_data(arg, data, len)) {
            std::string message(type_name(arg.type_));
            message.append(" is not support hex");
            return make_error(context, message);
        }
        std::string encoded = hex_encode(data, len);
        JsValue out = make_heap_string_value(context.runtime(), encoded);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class BinaryFromHexFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        std::string text;
        if (!get_utf8_string(context.arg_value(0), text)) {
            std::string message(type_name(context.arg_value(0).type_));
            message.append(" is not support hex");
            return make_error(context, message);
        }
        std::vector<std::uint8_t> decoded;
        if (!hex_decode(text, decoded)) {
            return make_error(context, "invalid hex string");
        }
        JsValue out = make_heap_binary_value(context.runtime(), decoded.data(), decoded.size());
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class BinaryUtf8BytesFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        std::string text = jsonutil_to_string(context.arg_value(0));
        JsValue out = make_heap_binary_value(context.runtime(),
                                             reinterpret_cast<const std::uint8_t *>(text.data()),
                                             text.size());
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class HashCrc32Func final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_integer(0);
        }
        std::string text = as_text(context.arg_value(0), "");
        if (text.empty()) {
            return JsValue::make_integer(0);
        }
        std::uint32_t value = crc32(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        return JsValue::make_integer(static_cast<std::int64_t>(value));
    }
};

class HashMd5Func final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        const JsValue &arg = context.arg_value(0);
        if (is_string_type(arg)) {
            std::string text;
            if (!get_utf8_string(arg, text)) {
                return make_error(context, "invalid utf-8");
            }
            auto digest = md5_digest(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
            std::string hex = hex_encode(digest.data(), digest.size());
            JsValue out = make_heap_string_value(context.runtime(), hex);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        if (is_binary_type(arg)) {
            const std::uint8_t *data = nullptr;
            std::size_t len = 0;
            if (!get_binary_data(arg, data, len)) {
                return make_error(context, "invalid binary");
            }
            auto digest = md5_digest(data, len);
            std::string hex = hex_encode(digest.data(), digest.size());
            JsValue out = make_heap_string_value(context.runtime(), hex);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        return make_type_error(context, "md5 not support ", arg);
    }
};

class HashSha1Func final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        const JsValue &arg = context.arg_value(0);
        if (is_string_type(arg)) {
            std::string text;
            if (!get_utf8_string(arg, text)) {
                return make_error(context, "invalid utf-8");
            }
            auto digest = sha1_digest(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
            std::string hex = hex_encode(digest.data(), digest.size());
            JsValue out = make_heap_string_value(context.runtime(), hex);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        if (is_binary_type(arg)) {
            const std::uint8_t *data = nullptr;
            std::size_t len = 0;
            if (!get_binary_data(arg, data, len)) {
                return make_error(context, "invalid binary");
            }
            auto digest = sha1_digest(data, len);
            std::string hex = hex_encode(digest.data(), digest.size());
            JsValue out = make_heap_string_value(context.runtime(), hex);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        return make_type_error(context, "sha1 not support ", arg);
    }
};

class HashSha256Func final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_undefined();
        }
        const JsValue &arg = context.arg_value(0);
        if (is_string_type(arg)) {
            std::string text;
            if (!get_utf8_string(arg, text)) {
                return make_error(context, "invalid utf-8");
            }
            auto digest = sha256_digest(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
            std::string hex = hex_encode(digest.data(), digest.size());
            JsValue out = make_heap_string_value(context.runtime(), hex);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        if (is_binary_type(arg)) {
            const std::uint8_t *data = nullptr;
            std::size_t len = 0;
            if (!get_binary_data(arg, data, len)) {
                return make_error(context, "invalid binary");
            }
            auto digest = sha256_digest(data, len);
            std::string hex = hex_encode(digest.data(), digest.size());
            JsValue out = make_heap_string_value(context.runtime(), hex);
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        return make_type_error(context, "sha256 not support ", arg);
    }
};

class RandRandomFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        std::int64_t bound = 1000;
        if (context.arg_count() >= 1) {
            const JsValue &arg = context.arg_value(0);
            if (!is_number_type(arg)) {
                return make_error(context, "random argument must be number");
            }
            bound = to_int64_default(arg);
        }
        if (bound <= 0) {
            return make_error(context, "random argument must be number");
        }
        static thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<std::int64_t> dist(0, bound - 1);
        return JsValue::make_integer(dist(rng));
    }
};

class RandCanaryFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            return JsValue::make_boolean(false);
        }
        std::int64_t ratio = to_int64_default(context.arg_value(0), 0);
        if (ratio <= 0) {
            return JsValue::make_boolean(false);
        }
        if (ratio >= 100) {
            return JsValue::make_boolean(true);
        }
        if (context.arg_count() == 1) {
            static thread_local std::mt19937_64 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, 99);
            return JsValue::make_boolean(dist(rng) < ratio);
        }
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 1; i < context.arg_count(); ++i) {
            std::string text = as_text(context.arg_value(i), "");
            if (text.empty()) {
                continue;
            }
            crc = crc32_update(crc, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        }
        std::uint32_t value = crc32_finish(crc);
        return JsValue::make_boolean((value % 100u) < static_cast<std::uint32_t>(ratio));
    }
};

class TimeNowFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        auto now = std::chrono::system_clock::now();
        if (context.arg_count() == 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            return JsValue::make_integer(static_cast<std::int64_t>(ms));
        }
        std::string pattern;
        if (!get_utf8_string(context.arg_value(0), pattern)) {
            std::string message = "now function valid format: ";
            message.append(as_text(context.arg_value(0), ""));
            return make_error(context, message);
        }
        std::string out;
        if (!format_time(pattern, now, true, out)) {
            std::string message = "now function valid format: ";
            message.append(pattern);
            return make_error(context, message);
        }
        JsValue result = make_heap_string_value(context.runtime(), out);
        if (result.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return result;
    }
};

class TimeFormatFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() == 0) {
            std::string out = format_rfc1123(std::chrono::system_clock::now());
            JsValue result = make_heap_string_value(context.runtime(), out);
            if (result.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return result;
        }
        std::string pattern;
        if (!get_utf8_string(context.arg_value(0), pattern)) {
            std::string message = "now function valid format: ";
            message.append(as_text(context.arg_value(0), ""));
            return make_error(context, message);
        }
        std::chrono::system_clock::time_point tp = std::chrono::system_clock::now();
        if (context.arg_count() > 1 && is_number_type(context.arg_value(1))) {
            std::int64_t ms = to_int64_default(context.arg_value(1),
                                               std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   tp.time_since_epoch())
                                                   .count());
            tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
        }
        std::string out;
        if (!format_time(pattern, tp, true, out)) {
            std::string message = "now function valid format: ";
            message.append(pattern);
            return make_error(context, message);
        }
        JsValue result = make_heap_string_value(context.runtime(), out);
        if (result.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return result;
    }
};

class UrlEncodeComponentFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 1) {
            return make_error(context, "encode component require at least one argument");
        }
        std::string input;
        if (!get_utf8_string(context.arg_value(0), input)) {
            return make_error(context, "encode component require text value");
        }
        if (input.empty()) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::string encoded;
        url_encode(input, encoded);
        JsValue out = make_heap_string_value(context.runtime(), encoded);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class UrlDecodeComponentFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 1) {
            return make_error(context, "decode component require at least one argument");
        }
        std::string input;
        if (!get_utf8_string(context.arg_value(0), input)) {
            return make_error(context, "decode component require text value");
        }
        if (input.empty()) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::string decoded;
        if (!url_decode(input, decoded)) {
            return make_error(context, "decode component invalid encoding");
        }
        JsValue out = make_heap_string_value(context.runtime(), decoded);
        if (out.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return out;
    }
};

class UrlParseQueryFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 1) {
            return make_error(context, "parse query require at least one argument");
        }
        std::string input;
        if (!get_utf8_string(context.arg_value(0), input)) {
            return make_error(context, "parse query require text value");
        }
        ScriptRuntime &runtime = context.runtime();
        JsValue obj_val = JsValue::make_object(runtime.heap(), 0);
        if (obj_val.type_ != JsNodeType::Object) {
            return make_oom_error(context);
        }
        GcRootGuard guard(runtime, &obj_val);
        auto *obj = reinterpret_cast<GcObject *>(obj_val.gc);
        if (!obj) {
            return obj_val;
        }
        if (input.empty()) {
            return obj_val;
        }
        std::size_t start = 0;
        while (start <= input.size()) {
            std::size_t end = input.find('&', start);
            if (end == std::string::npos) {
                end = input.size();
            }
            if (end > start) {
                std::string part = input.substr(start, end - start);
                std::size_t eq = part.find('=');
                std::string key_raw = eq == std::string::npos ? part : part.substr(0, eq);
                std::string val_raw = eq == std::string::npos ? "" : part.substr(eq + 1);
                std::string key;
                std::string value;
                if (!url_decode(key_raw, key) || !url_decode(val_raw, value)) {
                    return make_error(context, "parse query invalid encoding");
                }
                if (!key.empty()) {
                    auto *key_str = runtime.alloc_with_gc(key.size(), [&]() {
                        return fiber::json::gc_new_string(&runtime.heap(), key.data(), key.size());
                    });
                    if (!key_str) {
                        return make_oom_error(context);
                    }
                    JsValue value_val = make_heap_string_value(runtime, value);
                    if (value_val.type_ == JsNodeType::Undefined) {
                        return make_oom_error(context);
                    }
                    const JsValue *existing = fiber::json::gc_object_get(obj, key_str);
                    if (!existing) {
                        if (!fiber::json::gc_object_set(&runtime.heap(), obj, key_str, value_val)) {
                            return make_oom_error(context);
                        }
                    } else if (existing->type_ == JsNodeType::Array) {
                        auto *arr = reinterpret_cast<GcArray *>(existing->gc);
                        if (!arr || !fiber::json::gc_array_push(&runtime.heap(), arr, value_val)) {
                            return make_oom_error(context);
                        }
                    } else {
                        JsValue array = JsValue::make_array(runtime.heap(), 0);
                        if (array.type_ != JsNodeType::Array) {
                            return make_oom_error(context);
                        }
                        auto *arr = reinterpret_cast<GcArray *>(array.gc);
                        if (!fiber::json::gc_array_push(&runtime.heap(), arr, *existing) ||
                            !fiber::json::gc_array_push(&runtime.heap(), arr, value_val)) {
                            return make_oom_error(context);
                        }
                        if (!fiber::json::gc_object_set(&runtime.heap(), obj, key_str, array)) {
                            return make_oom_error(context);
                        }
                    }
                }
            }
            if (end == input.size()) {
                break;
            }
            start = end + 1;
        }
        return obj_val;
    }
};

class UrlBuildQueryFunc final : public Library::Function {
public:
    FunctionResult call(ExecutionContext &context) override {
        if (context.arg_count() < 1) {
            return make_error(context, "build query require at least one argument");
        }
        const JsValue &val = context.arg_value(0);
        if (val.type_ == JsNodeType::Undefined || val.type_ == JsNodeType::Null) {
            return val;
        }
        if (val.type_ != JsNodeType::Object) {
            return make_error(context, "build query require object value");
        }
        auto *obj = reinterpret_cast<const GcObject *>(val.gc);
        if (!obj || obj->size == 0) {
            JsValue out = make_heap_string_value(context.runtime(), "");
            if (out.type_ == JsNodeType::Undefined) {
                return make_oom_error(context);
            }
            return out;
        }
        std::string out;
        std::string key;
        std::string value;
        std::string encoded_key;
        std::string encoded_val;
        int32_t cursor = obj->head;
        while (cursor != -1) {
            const GcObjectEntry &entry = obj->entries[cursor];
            if (!entry.occupied || !entry.key) {
                cursor = entry.next_order;
                continue;
            }
            key.clear();
            if (!fiber::json::gc_string_to_utf8(entry.key, key)) {
                cursor = entry.next_order;
                continue;
            }
            if (entry.value.type_ == JsNodeType::Array) {
                auto *arr = reinterpret_cast<const GcArray *>(entry.value.gc);
                if (arr) {
                    for (std::size_t i = 0; i < arr->size; ++i) {
                        const JsValue *elem = fiber::json::gc_array_get(arr, i);
                        if (!elem) {
                            continue;
                        }
                        value = jsonutil_to_string(*elem);
                        url_encode(key, encoded_key);
                        url_encode(value, encoded_val);
                        out.append(encoded_key);
                        out.push_back('=');
                        out.append(encoded_val);
                        out.push_back('&');
                    }
                }
            } else {
                value = jsonutil_to_string(entry.value);
                url_encode(key, encoded_key);
                url_encode(value, encoded_val);
                out.append(encoded_key);
                out.push_back('=');
                out.append(encoded_val);
                out.push_back('&');
            }
            cursor = entry.next_order;
        }
        if (!out.empty() && out.back() == '&') {
            out.pop_back();
        }
        JsValue result = make_heap_string_value(context.runtime(), out);
        if (result.type_ == JsNodeType::Undefined) {
            return make_oom_error(context);
        }
        return result;
    }
};

} // namespace

void register_std_library(StdLibrary &library) {
    static LengthFunc length_func;
    static IncludesFunc includes_func;
    static ArrayJoinFunc array_join;
    static ArrayPopFunc array_pop;
    static ArrayPushFunc array_push;
    static ObjectAssignFunc obj_assign;
    static ObjectKeysFunc obj_keys;
    static ObjectValuesFunc obj_values;
    static ObjectDeletePropsFunc obj_delete;
    static HasPrefixFunc strings_has_prefix;
    static HasSuffixFunc strings_has_suffix;
    static ToLowerFunc strings_to_lower;
    static ToUpperFunc strings_to_upper;
    static TrimFunc strings_trim;
    static TrimLeftFunc strings_trim_left;
    static TrimRightFunc strings_trim_right;
    static SplitFunc strings_split;
    static FindAllFunc strings_find_all;
    static ContainsFunc strings_contains;
    static ContainsAnyFunc strings_contains_any;
    static IndexFunc strings_index;
    static IndexAnyFunc strings_index_any;
    static LastIndexFunc strings_last_index;
    static LastIndexAnyFunc strings_last_index_any;
    static RepeatFunc strings_repeat;
    static MatchFunc strings_match;
    static SubstringFunc strings_substring;
    static ToStringFunc strings_to_string;
    static JsonParseFunc json_parse;
    static JsonStringifyFunc json_stringify;
    static MathFloorFunc math_floor;
    static MathAbsFunc math_abs;
    static BinaryBase64EncodeFunc bin_b64_encode;
    static BinaryBase64DecodeFunc bin_b64_decode;
    static BinaryHexFunc bin_hex;
    static BinaryFromHexFunc bin_from_hex;
    static BinaryUtf8BytesFunc bin_utf8;
    static HashCrc32Func hash_crc32;
    static HashMd5Func hash_md5;
    static HashSha1Func hash_sha1;
    static HashSha256Func hash_sha256;
    static RandRandomFunc rand_random;
    static RandCanaryFunc rand_canary;
    static TimeNowFunc time_now;
    static TimeFormatFunc time_format;
    static UrlEncodeComponentFunc url_encode_component;
    static UrlDecodeComponentFunc url_decode_component;
    static UrlParseQueryFunc url_parse_query;
    static UrlBuildQueryFunc url_build_query;

    library.register_func("length", &length_func);
    library.register_func("includes", &includes_func);
    library.register_func("array.join", &array_join);
    library.register_func("array.pop", &array_pop);
    library.register_func("array.push", &array_push);
    library.register_func("Object.assign", &obj_assign);
    library.register_func("Object.keys", &obj_keys);
    library.register_func("Object.values", &obj_values);
    library.register_func("Object.deleteProperties", &obj_delete);
    library.register_func("strings.hasPrefix", &strings_has_prefix);
    library.register_func("strings.hasSuffix", &strings_has_suffix);
    library.register_func("strings.toLower", &strings_to_lower);
    library.register_func("strings.toUpper", &strings_to_upper);
    library.register_func("strings.trim", &strings_trim);
    library.register_func("strings.trimLeft", &strings_trim_left);
    library.register_func("strings.trimRight", &strings_trim_right);
    library.register_func("strings.split", &strings_split);
    library.register_func("strings.findAll", &strings_find_all);
    library.register_func("strings.contains", &strings_contains);
    library.register_func("strings.contains_any", &strings_contains_any);
    library.register_func("strings.index", &strings_index);
    library.register_func("strings.indexAny", &strings_index_any);
    library.register_func("strings.lastIndex", &strings_last_index);
    library.register_func("strings.lastIndexAny", &strings_last_index_any);
    library.register_func("strings.repeat", &strings_repeat);
    library.register_func("strings.match", &strings_match);
    library.register_func("strings.substring", &strings_substring);
    library.register_func("strings.toString", &strings_to_string);
    library.register_func("JSON.parse", &json_parse);
    library.register_func("JSON.stringify", &json_stringify);
    library.register_func("math.floor", &math_floor);
    library.register_func("math.abs", &math_abs);
    library.register_func("binary.base64Encode", &bin_b64_encode);
    library.register_func("binary.base64Decode", &bin_b64_decode);
    library.register_func("binary.hex", &bin_hex);
    library.register_func("binary.fromHex", &bin_from_hex);
    library.register_func("binary.getUtf8Bytes", &bin_utf8);
    library.register_func("hash.crc32", &hash_crc32);
    library.register_func("hash.md5", &hash_md5);
    library.register_func("hash.sha1", &hash_sha1);
    library.register_func("hash.sha256", &hash_sha256);
    library.register_func("rand.random", &rand_random);
    library.register_func("rand.canary", &rand_canary);
    library.register_func("time.now", &time_now);
    library.register_func("time.format", &time_format);
    library.register_func("URL.encodeComponent", &url_encode_component);
    library.register_func("URL.decodeComponent", &url_decode_component);
    library.register_func("URL.parseQuery", &url_parse_query);
    library.register_func("URL.buildQuery", &url_build_query);
}

} // namespace fiber::script::std_lib
