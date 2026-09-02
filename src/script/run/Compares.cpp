#include <fiber/script/run/Compares.h>

#include <fiber/common/json/Utf.h>
#include <fiber/script/gc/GcInternal.h>
#include "script/gc/Wtf8.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace fiber::script::run {

namespace {

bool is_string_type(fiber::script::JsNodeType type) noexcept { return type == fiber::script::JsNodeType::String; }

bool is_number_type(fiber::script::JsNodeType type) noexcept {
    return type == fiber::script::JsNodeType::Integer || type == fiber::script::JsNodeType::Float;
}

bool is_numeric_like(fiber::script::JsNodeType type) noexcept {
    return type == fiber::script::JsNodeType::Integer || type == fiber::script::JsNodeType::Float ||
           type == fiber::script::JsNodeType::Boolean || type == fiber::script::JsNodeType::Null;
}

const fiber::script::GcString *as_heap_string(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::String
                   ? fiber::script::js_value_heap_ptr<const fiber::script::GcString>(value)
                   : nullptr;
}

bool to_number(const fiber::script::JsValue &value, double &out) noexcept {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Integer:
            out = static_cast<double>(fiber::script::js_value_int64(value));
            return true;
        case fiber::script::JsNodeType::Float:
            out = fiber::script::js_value_double(value);
            return true;
        case fiber::script::JsNodeType::Boolean:
            out = fiber::script::js_value_bool(value) ? 1.0 : 0.0;
            return true;
        case fiber::script::JsNodeType::Null:
            out = 0.0;
            return true;
        default:
            return false;
    }
}

bool is_truthy(const fiber::script::JsValue &value) noexcept {
    switch (fiber::script::js_value_type(value)) {
        case fiber::script::JsNodeType::Undefined:
        case fiber::script::JsNodeType::Null:
            return false;
        case fiber::script::JsNodeType::Boolean:
            return fiber::script::js_value_bool(value);
        case fiber::script::JsNodeType::Integer:
            return fiber::script::js_value_int64(value) != 0;
        case fiber::script::JsNodeType::Float:
            return fiber::script::js_value_double(value) != 0.0 && !std::isnan(fiber::script::js_value_double(value));
        case fiber::script::JsNodeType::String:
            if (fiber::script::js_value_is_borrowed_string(value)) {
                return fiber::script::js_value_native_string(value).len > 0;
            }
            if (auto *str = as_heap_string(value)) {
                return str->utf16_len > 0;
            }
            return false;
        case fiber::script::JsNodeType::Binary:
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
            return true;
    }
    return false;
}

enum class StringKind : std::uint8_t {
    HeapWtf8,
    NativeUtf8,
};

bool ascii_is_space(char16_t ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

bool ascii_is_digit(char16_t ch) noexcept { return ch >= '0' && ch <= '9'; }

int ascii_hex_digit(char16_t ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return static_cast<int>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<int>(ch - 'a') + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<int>(ch - 'A') + 10;
    }
    return -1;
}

bool ascii_eq_ci(char16_t lhs, char rhs) noexcept {
    if (lhs >= 'A' && lhs <= 'Z') {
        lhs = static_cast<char16_t>(lhs + ('a' - 'A'));
    }
    if (rhs >= 'A' && rhs <= 'Z') {
        rhs = static_cast<char>(rhs + ('a' - 'A'));
    }
    return lhs == static_cast<unsigned char>(rhs);
}

void clamp_exp_add(int &exp, int delta) noexcept {
    constexpr int kExpCap = 100000;
    if (delta > 0) {
        exp = exp >= kExpCap ? kExpCap : exp + 1;
    } else if (delta < 0) {
        exp = exp <= -kExpCap ? -kExpCap : exp - 1;
    }
}

void clamp_exp_add_many(int &exp, int delta) noexcept {
    constexpr int kExpCap = 100000;
    if (delta > 0) {
        exp = exp > kExpCap - delta ? kExpCap : exp + delta;
    } else if (delta < 0) {
        exp = exp < -kExpCap - delta ? -kExpCap : exp + delta;
    }
}

struct StringCursor {
    StringKind kind = StringKind::NativeUtf8;
    const char *utf8 = nullptr;
    std::size_t len = 0;
    std::size_t pos = 0;
    bool has_pending = false;
    char16_t pending = 0;
    bool malformed = false;
    fiber::script::gc_detail::Wtf8Cursor wtf8;
};

bool init_heap_string_cursor(const fiber::script::GcString *str, StringCursor &out) noexcept {
    out = {};
    if (!str) {
        return false;
    }
    out.kind = StringKind::HeapWtf8;
    out.wtf8.data = fiber::script::gc_string_wtf8_data(str);
    out.wtf8.len = fiber::script::gc_string_byte_len(str);
    return true;
}

bool init_native_string_cursor(fiber::script::NativeStr native, StringCursor &out) noexcept {
    out = {};
    out.kind = StringKind::NativeUtf8;
    out.utf8 = native.data;
    out.len = native.len;
    return out.len == 0 || out.utf8;
}

bool init_string_cursor(const fiber::script::JsValue &value, StringCursor &out) noexcept {
    if (fiber::script::js_value_type(value) != fiber::script::JsNodeType::String) {
        return false;
    }
    if (!fiber::script::js_value_is_borrowed_string(value)) {
        return init_heap_string_cursor(as_heap_string(value), out);
    }
    return init_native_string_cursor(fiber::script::js_value_native_string(value), out);
}

bool cursor_next(StringCursor &cursor, char16_t &unit) noexcept {
    switch (cursor.kind) {
        case StringKind::HeapWtf8: {
            const bool has_unit = fiber::script::gc_detail::wtf8_next_utf16_unit(cursor.wtf8, unit);
            cursor.malformed = cursor.wtf8.malformed;
            return has_unit;
        }
        case StringKind::NativeUtf8: {
            if (cursor.has_pending) {
                unit = cursor.pending;
                cursor.has_pending = false;
                return true;
            }
            if (cursor.pos >= cursor.len) {
                return false;
            }
            std::uint32_t codepoint = 0;
            if (!fiber::json::utf8_next_codepoint(cursor.utf8, cursor.len, cursor.pos, codepoint)) {
                cursor.malformed = true;
                return false;
            }
            if (codepoint <= 0xFFFF) {
                unit = static_cast<char16_t>(codepoint);
                return true;
            }
            std::uint32_t value = codepoint - 0x10000;
            unit = static_cast<char16_t>(0xD800 + (value >> 10));
            cursor.pending = static_cast<char16_t>(0xDC00 + (value & 0x3FF));
            cursor.has_pending = true;
            return true;
        }
    }
    return false;
}

struct UnitReader {
    StringCursor cursor;
    bool has_saved = false;
    char16_t saved = 0;
};

bool read_unit(UnitReader &reader, char16_t &unit) noexcept {
    if (reader.has_saved) {
        unit = reader.saved;
        reader.has_saved = false;
        return true;
    }
    return cursor_next(reader.cursor, unit);
}

void unread_unit(UnitReader &reader, char16_t unit) noexcept {
    reader.saved = unit;
    reader.has_saved = true;
}

bool read_next(UnitReader &reader, char16_t &unit, bool &has_unit) noexcept {
    has_unit = read_unit(reader, unit);
    return has_unit || !reader.cursor.malformed;
}

bool drain_remaining(UnitReader &reader) noexcept {
    char16_t unit = 0;
    while (read_unit(reader, unit)) {
    }
    return !reader.cursor.malformed;
}

bool finish_nan(UnitReader &reader, double &out) noexcept {
    if (!drain_remaining(reader)) {
        return false;
    }
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
}

bool trailing_spaces_only(UnitReader &reader, bool &only_spaces) noexcept {
    only_spaces = true;
    char16_t unit = 0;
    while (read_unit(reader, unit)) {
        if (!ascii_is_space(unit)) {
            only_spaces = false;
            return drain_remaining(reader);
        }
    }
    return !reader.cursor.malformed;
}

bool read_ascii_ci(UnitReader &reader, char expected) noexcept {
    char16_t unit = 0;
    return read_unit(reader, unit) && ascii_eq_ci(unit, expected);
}

bool parse_nan(UnitReader &reader, double &out) noexcept {
    if (!read_ascii_ci(reader, 'a') || !read_ascii_ci(reader, 'n')) {
        return finish_nan(reader, out);
    }
    bool only_spaces = true;
    if (!trailing_spaces_only(reader, only_spaces)) {
        return false;
    }
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
}

bool parse_infinity(UnitReader &reader, double sign, double &out) noexcept {
    if (!read_ascii_ci(reader, 'n') || !read_ascii_ci(reader, 'f')) {
        return finish_nan(reader, out);
    }
    char16_t unit = 0;
    bool has_unit = false;
    if (!read_next(reader, unit, has_unit)) {
        return false;
    }
    if (has_unit && ascii_eq_ci(unit, 'i')) {
        if (!read_ascii_ci(reader, 'n') || !read_ascii_ci(reader, 'i') || !read_ascii_ci(reader, 't') ||
            !read_ascii_ci(reader, 'y')) {
            return finish_nan(reader, out);
        }
    } else if (has_unit) {
        unread_unit(reader, unit);
    }
    bool only_spaces = true;
    if (!trailing_spaces_only(reader, only_spaces)) {
        return false;
    }
    if (!only_spaces) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    out = sign * std::numeric_limits<double>::infinity();
    return true;
}

// Correctly-rounded 10^e for exponents in [kPow10MinExp, kPow10MaxExp], folded
// from decimal literals at compile time. Replaces the libm pow() call whose
// newer symbol versions raise the binary's minimum glibc requirement. Values
// match the round-half-even result of the exact decimal power (glibc pow()
// itself is up to 1 ulp off at e=23 and e=210). Exponents below the table
// underflow to zero and are rejected by the range guard first.
constexpr int kPow10MinExp = -323;
constexpr int kPow10MaxExp = 308;
constexpr double kPow10Table[kPow10MaxExp - kPow10MinExp + 1] = {
        1e-323, 1e-322, 1e-321, 1e-320, 1e-319, 1e-318, 1e-317, 1e-316, 1e-315, 1e-314, 1e-313, 1e-312, 1e-311, 1e-310,
        1e-309, 1e-308, 1e-307, 1e-306, 1e-305, 1e-304, 1e-303, 1e-302, 1e-301, 1e-300, 1e-299, 1e-298, 1e-297, 1e-296,
        1e-295, 1e-294, 1e-293, 1e-292, 1e-291, 1e-290, 1e-289, 1e-288, 1e-287, 1e-286, 1e-285, 1e-284, 1e-283, 1e-282,
        1e-281, 1e-280, 1e-279, 1e-278, 1e-277, 1e-276, 1e-275, 1e-274, 1e-273, 1e-272, 1e-271, 1e-270, 1e-269, 1e-268,
        1e-267, 1e-266, 1e-265, 1e-264, 1e-263, 1e-262, 1e-261, 1e-260, 1e-259, 1e-258, 1e-257, 1e-256, 1e-255, 1e-254,
        1e-253, 1e-252, 1e-251, 1e-250, 1e-249, 1e-248, 1e-247, 1e-246, 1e-245, 1e-244, 1e-243, 1e-242, 1e-241, 1e-240,
        1e-239, 1e-238, 1e-237, 1e-236, 1e-235, 1e-234, 1e-233, 1e-232, 1e-231, 1e-230, 1e-229, 1e-228, 1e-227, 1e-226,
        1e-225, 1e-224, 1e-223, 1e-222, 1e-221, 1e-220, 1e-219, 1e-218, 1e-217, 1e-216, 1e-215, 1e-214, 1e-213, 1e-212,
        1e-211, 1e-210, 1e-209, 1e-208, 1e-207, 1e-206, 1e-205, 1e-204, 1e-203, 1e-202, 1e-201, 1e-200, 1e-199, 1e-198,
        1e-197, 1e-196, 1e-195, 1e-194, 1e-193, 1e-192, 1e-191, 1e-190, 1e-189, 1e-188, 1e-187, 1e-186, 1e-185, 1e-184,
        1e-183, 1e-182, 1e-181, 1e-180, 1e-179, 1e-178, 1e-177, 1e-176, 1e-175, 1e-174, 1e-173, 1e-172, 1e-171, 1e-170,
        1e-169, 1e-168, 1e-167, 1e-166, 1e-165, 1e-164, 1e-163, 1e-162, 1e-161, 1e-160, 1e-159, 1e-158, 1e-157, 1e-156,
        1e-155, 1e-154, 1e-153, 1e-152, 1e-151, 1e-150, 1e-149, 1e-148, 1e-147, 1e-146, 1e-145, 1e-144, 1e-143, 1e-142,
        1e-141, 1e-140, 1e-139, 1e-138, 1e-137, 1e-136, 1e-135, 1e-134, 1e-133, 1e-132, 1e-131, 1e-130, 1e-129, 1e-128,
        1e-127, 1e-126, 1e-125, 1e-124, 1e-123, 1e-122, 1e-121, 1e-120, 1e-119, 1e-118, 1e-117, 1e-116, 1e-115, 1e-114,
        1e-113, 1e-112, 1e-111, 1e-110, 1e-109, 1e-108, 1e-107, 1e-106, 1e-105, 1e-104, 1e-103, 1e-102, 1e-101, 1e-100,
        1e-99,  1e-98,  1e-97,  1e-96,  1e-95,  1e-94,  1e-93,  1e-92,  1e-91,  1e-90,  1e-89,  1e-88,  1e-87,  1e-86,
        1e-85,  1e-84,  1e-83,  1e-82,  1e-81,  1e-80,  1e-79,  1e-78,  1e-77,  1e-76,  1e-75,  1e-74,  1e-73,  1e-72,
        1e-71,  1e-70,  1e-69,  1e-68,  1e-67,  1e-66,  1e-65,  1e-64,  1e-63,  1e-62,  1e-61,  1e-60,  1e-59,  1e-58,
        1e-57,  1e-56,  1e-55,  1e-54,  1e-53,  1e-52,  1e-51,  1e-50,  1e-49,  1e-48,  1e-47,  1e-46,  1e-45,  1e-44,
        1e-43,  1e-42,  1e-41,  1e-40,  1e-39,  1e-38,  1e-37,  1e-36,  1e-35,  1e-34,  1e-33,  1e-32,  1e-31,  1e-30,
        1e-29,  1e-28,  1e-27,  1e-26,  1e-25,  1e-24,  1e-23,  1e-22,  1e-21,  1e-20,  1e-19,  1e-18,  1e-17,  1e-16,
        1e-15,  1e-14,  1e-13,  1e-12,  1e-11,  1e-10,  1e-9,   1e-8,   1e-7,   1e-6,   1e-5,   1e-4,   1e-3,   1e-2,
        1e-1,   1e0,    1e1,    1e2,    1e3,    1e4,    1e5,    1e6,    1e7,    1e8,    1e9,    1e10,   1e11,   1e12,
        1e13,   1e14,   1e15,   1e16,   1e17,   1e18,   1e19,   1e20,   1e21,   1e22,   1e23,   1e24,   1e25,   1e26,
        1e27,   1e28,   1e29,   1e30,   1e31,   1e32,   1e33,   1e34,   1e35,   1e36,   1e37,   1e38,   1e39,   1e40,
        1e41,   1e42,   1e43,   1e44,   1e45,   1e46,   1e47,   1e48,   1e49,   1e50,   1e51,   1e52,   1e53,   1e54,
        1e55,   1e56,   1e57,   1e58,   1e59,   1e60,   1e61,   1e62,   1e63,   1e64,   1e65,   1e66,   1e67,   1e68,
        1e69,   1e70,   1e71,   1e72,   1e73,   1e74,   1e75,   1e76,   1e77,   1e78,   1e79,   1e80,   1e81,   1e82,
        1e83,   1e84,   1e85,   1e86,   1e87,   1e88,   1e89,   1e90,   1e91,   1e92,   1e93,   1e94,   1e95,   1e96,
        1e97,   1e98,   1e99,   1e100,  1e101,  1e102,  1e103,  1e104,  1e105,  1e106,  1e107,  1e108,  1e109,  1e110,
        1e111,  1e112,  1e113,  1e114,  1e115,  1e116,  1e117,  1e118,  1e119,  1e120,  1e121,  1e122,  1e123,  1e124,
        1e125,  1e126,  1e127,  1e128,  1e129,  1e130,  1e131,  1e132,  1e133,  1e134,  1e135,  1e136,  1e137,  1e138,
        1e139,  1e140,  1e141,  1e142,  1e143,  1e144,  1e145,  1e146,  1e147,  1e148,  1e149,  1e150,  1e151,  1e152,
        1e153,  1e154,  1e155,  1e156,  1e157,  1e158,  1e159,  1e160,  1e161,  1e162,  1e163,  1e164,  1e165,  1e166,
        1e167,  1e168,  1e169,  1e170,  1e171,  1e172,  1e173,  1e174,  1e175,  1e176,  1e177,  1e178,  1e179,  1e180,
        1e181,  1e182,  1e183,  1e184,  1e185,  1e186,  1e187,  1e188,  1e189,  1e190,  1e191,  1e192,  1e193,  1e194,
        1e195,  1e196,  1e197,  1e198,  1e199,  1e200,  1e201,  1e202,  1e203,  1e204,  1e205,  1e206,  1e207,  1e208,
        1e209,  1e210,  1e211,  1e212,  1e213,  1e214,  1e215,  1e216,  1e217,  1e218,  1e219,  1e220,  1e221,  1e222,
        1e223,  1e224,  1e225,  1e226,  1e227,  1e228,  1e229,  1e230,  1e231,  1e232,  1e233,  1e234,  1e235,  1e236,
        1e237,  1e238,  1e239,  1e240,  1e241,  1e242,  1e243,  1e244,  1e245,  1e246,  1e247,  1e248,  1e249,  1e250,
        1e251,  1e252,  1e253,  1e254,  1e255,  1e256,  1e257,  1e258,  1e259,  1e260,  1e261,  1e262,  1e263,  1e264,
        1e265,  1e266,  1e267,  1e268,  1e269,  1e270,  1e271,  1e272,  1e273,  1e274,  1e275,  1e276,  1e277,  1e278,
        1e279,  1e280,  1e281,  1e282,  1e283,  1e284,  1e285,  1e286,  1e287,  1e288,  1e289,  1e290,  1e291,  1e292,
        1e293,  1e294,  1e295,  1e296,  1e297,  1e298,  1e299,  1e300,  1e301,  1e302,  1e303,  1e304,  1e305,  1e306,
        1e307,  1e308,
};

double scale_decimal(double mantissa, int exp) noexcept {
    if (mantissa == 0.0) {
        return mantissa;
    }
    if (exp > kPow10MaxExp) {
        return std::numeric_limits<double>::infinity();
    }
    if (exp < kPow10MinExp) {
        return 0.0;
    }
    return mantissa * kPow10Table[static_cast<std::size_t>(exp - kPow10MinExp)];
}

double scale_binary(double mantissa, int exp) noexcept {
    if (mantissa == 0.0) {
        return mantissa;
    }
    if (exp > 1024) {
        return std::numeric_limits<double>::infinity();
    }
    if (exp < -1100) {
        return 0.0;
    }
    return std::ldexp(mantissa, exp);
}

bool parse_hex_number(UnitReader &reader, double sign, double &out) noexcept {
    constexpr int kMaxSignificantHexDigits = 13;
    double mantissa = 0.0;
    int significant_digits = 0;
    int binary_exp = 0;
    bool saw_digit = false;
    bool saw_nonzero = false;
    auto add_digit = [&](int digit, bool after_dot) noexcept {
        saw_digit = true;
        if (digit != 0 || saw_nonzero) {
            saw_nonzero = true;
            if (significant_digits < kMaxSignificantHexDigits) {
                mantissa = mantissa * 16.0 + static_cast<double>(digit);
                ++significant_digits;
                if (after_dot) {
                    clamp_exp_add_many(binary_exp, -4);
                }
            } else if (!after_dot) {
                clamp_exp_add_many(binary_exp, 4);
            }
        } else if (after_dot) {
            clamp_exp_add_many(binary_exp, -4);
        }
    };

    char16_t unit = 0;
    bool has_unit = false;
    if (!read_next(reader, unit, has_unit)) {
        return false;
    }
    while (has_unit) {
        int digit = ascii_hex_digit(unit);
        if (digit < 0) {
            break;
        }
        add_digit(digit, false);
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
    }
    if (has_unit && unit == '.') {
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        while (has_unit) {
            int digit = ascii_hex_digit(unit);
            if (digit < 0) {
                break;
            }
            add_digit(digit, true);
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
    }
    if (!saw_digit) {
        return finish_nan(reader, out);
    }

    int exponent = 0;
    if (has_unit && (unit == 'p' || unit == 'P')) {
        int exponent_sign = 1;
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (has_unit && (unit == '+' || unit == '-')) {
            exponent_sign = unit == '-' ? -1 : 1;
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        if (!has_unit || !ascii_is_digit(unit)) {
            return finish_nan(reader, out);
        }
        while (has_unit && ascii_is_digit(unit)) {
            if (exponent < 100000) {
                exponent = exponent * 10 + static_cast<int>(unit - '0');
                if (exponent > 100000) {
                    exponent = 100000;
                }
            }
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        exponent *= exponent_sign;
    }

    if (has_unit) {
        if (!ascii_is_space(unit)) {
            return finish_nan(reader, out);
        }
        bool only_spaces = true;
        if (!trailing_spaces_only(reader, only_spaces)) {
            return false;
        }
        if (!only_spaces) {
            out = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
    }
    int total_exp = binary_exp;
    clamp_exp_add_many(total_exp, exponent);
    out = sign * scale_binary(mantissa, total_exp);
    return true;
}

bool string_cursor_to_number(StringCursor cursor, double &out) noexcept {
    constexpr int kMaxSignificantDigits = 18;
    UnitReader reader{cursor};
    char16_t unit = 0;
    bool has_unit = false;
    do {
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (!has_unit) {
            out = 0.0;
            return true;
        }
    } while (ascii_is_space(unit));

    double sign = 1.0;
    if (unit == '+' || unit == '-') {
        sign = unit == '-' ? -1.0 : 1.0;
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (!has_unit) {
            return finish_nan(reader, out);
        }
    }

    if (ascii_eq_ci(unit, 'n')) {
        return parse_nan(reader, out);
    }
    if (ascii_eq_ci(unit, 'i')) {
        return parse_infinity(reader, sign, out);
    }
    if (unit == '0') {
        char16_t next = 0;
        bool has_next = false;
        if (!read_next(reader, next, has_next)) {
            return false;
        }
        if (has_next && (next == 'x' || next == 'X')) {
            return parse_hex_number(reader, sign, out);
        }
        if (has_next) {
            unread_unit(reader, next);
        }
    }

    double mantissa = 0.0;
    int significant_digits = 0;
    int decimal_exp = 0;
    bool saw_digit = false;
    bool saw_nonzero = false;
    auto add_digit = [&](int digit, bool after_dot) noexcept {
        saw_digit = true;
        if (digit != 0 || saw_nonzero) {
            saw_nonzero = true;
            if (significant_digits < kMaxSignificantDigits) {
                mantissa = mantissa * 10.0 + static_cast<double>(digit);
                ++significant_digits;
                if (after_dot) {
                    clamp_exp_add(decimal_exp, -1);
                }
            } else if (!after_dot) {
                clamp_exp_add(decimal_exp, 1);
            }
        } else if (after_dot) {
            clamp_exp_add(decimal_exp, -1);
        }
    };

    while (has_unit && ascii_is_digit(unit)) {
        add_digit(static_cast<int>(unit - '0'), false);
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
    }
    if (has_unit && unit == '.') {
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        while (has_unit && ascii_is_digit(unit)) {
            add_digit(static_cast<int>(unit - '0'), true);
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
    }
    if (!saw_digit) {
        return finish_nan(reader, out);
    }

    int exponent = 0;
    if (has_unit && (unit == 'e' || unit == 'E')) {
        int exponent_sign = 1;
        if (!read_next(reader, unit, has_unit)) {
            return false;
        }
        if (has_unit && (unit == '+' || unit == '-')) {
            exponent_sign = unit == '-' ? -1 : 1;
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        if (!has_unit || !ascii_is_digit(unit)) {
            return finish_nan(reader, out);
        }
        while (has_unit && ascii_is_digit(unit)) {
            if (exponent < 100000) {
                exponent = exponent * 10 + static_cast<int>(unit - '0');
                if (exponent > 100000) {
                    exponent = 100000;
                }
            }
            if (!read_next(reader, unit, has_unit)) {
                return false;
            }
        }
        exponent *= exponent_sign;
    }

    if (has_unit) {
        if (!ascii_is_space(unit)) {
            return finish_nan(reader, out);
        }
        bool only_spaces = true;
        if (!trailing_spaces_only(reader, only_spaces)) {
            return false;
        }
        if (!only_spaces) {
            out = std::numeric_limits<double>::quiet_NaN();
            return true;
        }
    }
    int total_exp = decimal_exp;
    clamp_exp_add_many(total_exp, exponent);
    out = sign * scale_decimal(mantissa, total_exp);
    return true;
}

bool string_to_number(const fiber::script::JsValue &value, double &out) noexcept {
    StringCursor cursor;
    if (!init_string_cursor(value, cursor)) {
        return false;
    }
    return string_cursor_to_number(cursor, out);
}

bool compare_string_cursors(StringCursor lhs_cursor, StringCursor rhs_cursor, int &result) noexcept {
    while (true) {
        char16_t lhs_unit = 0;
        char16_t rhs_unit = 0;
        bool lhs_has = cursor_next(lhs_cursor, lhs_unit);
        bool rhs_has = cursor_next(rhs_cursor, rhs_unit);
        if (lhs_cursor.malformed || rhs_cursor.malformed) {
            return false;
        }
        if (!lhs_has || !rhs_has) {
            if (lhs_has) {
                result = 1;
            } else if (rhs_has) {
                result = -1;
            } else {
                result = 0;
            }
            return true;
        }
        if (lhs_unit < rhs_unit) {
            result = -1;
            return true;
        }
        if (lhs_unit > rhs_unit) {
            result = 1;
            return true;
        }
    }
}

bool compare_strings(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs, int &result) noexcept {
    if (!fiber::script::js_value_is_borrowed_string(lhs) && !fiber::script::js_value_is_borrowed_string(rhs) &&
        fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::String &&
        fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::String) {
        auto *lhs_str = as_heap_string(lhs);
        auto *rhs_str = as_heap_string(rhs);
        if (!lhs_str || !rhs_str) {
            return false;
        }
        if (fiber::script::gc_string_is_ascii(lhs_str) && fiber::script::gc_string_is_ascii(rhs_str)) {
            const std::size_t lhs_len = fiber::script::gc_string_byte_len(lhs_str);
            const std::size_t rhs_len = fiber::script::gc_string_byte_len(rhs_str);
            std::size_t min_len = lhs_len < rhs_len ? lhs_len : rhs_len;
            int cmp = 0;
            if (min_len > 0) {
                cmp = std::memcmp(fiber::script::gc_string_wtf8_data(lhs_str),
                                  fiber::script::gc_string_wtf8_data(rhs_str), min_len);
            }
            if (cmp == 0) {
                if (lhs_len < rhs_len) {
                    cmp = -1;
                } else if (lhs_len > rhs_len) {
                    cmp = 1;
                }
            }
            result = cmp;
            return true;
        }
    }

    StringCursor lhs_cursor;
    StringCursor rhs_cursor;
    if (!init_string_cursor(lhs, lhs_cursor) || !init_string_cursor(rhs, rhs_cursor)) {
        return false;
    }
    return compare_string_cursors(lhs_cursor, rhs_cursor, result);
}

bool heap_string_equals_native_utf8(const fiber::script::GcString *heap_str, fiber::script::NativeStr native) noexcept {
    StringCursor lhs_cursor;
    StringCursor rhs_cursor;
    if (!init_heap_string_cursor(heap_str, lhs_cursor) || !init_native_string_cursor(native, rhs_cursor)) {
        return false;
    }
    int cmp = 0;
    return compare_string_cursors(lhs_cursor, rhs_cursor, cmp) && cmp == 0;
}

double number_value(const fiber::script::JsValue &value) noexcept {
    return fiber::script::js_value_type(value) == fiber::script::JsNodeType::Integer
                   ? static_cast<double>(fiber::script::js_value_int64(value))
                   : fiber::script::js_value_double(value);
}

bool numbers_equal(double lhs, double rhs) noexcept {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    return lhs == rhs;
}

bool strict_equal(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs) noexcept {
    if (is_string_type(fiber::script::js_value_type(lhs)) && is_string_type(fiber::script::js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(fiber::script::js_value_type(lhs)) && is_number_type(fiber::script::js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (fiber::script::js_value_type(lhs) != fiber::script::js_value_type(rhs)) {
        return false;
    }
    switch (fiber::script::js_value_type(lhs)) {
        case fiber::script::JsNodeType::Undefined:
        case fiber::script::JsNodeType::Null:
            return true;
        case fiber::script::JsNodeType::Boolean:
            return fiber::script::js_value_bool(lhs) == fiber::script::js_value_bool(rhs);
        case fiber::script::JsNodeType::Integer:
            return fiber::script::js_value_int64(lhs) == fiber::script::js_value_int64(rhs);
        case fiber::script::JsNodeType::Float:
            return fiber::script::js_value_double(lhs) == fiber::script::js_value_double(rhs);
        case fiber::script::JsNodeType::Binary:
            if (fiber::script::js_value_is_borrowed_binary(lhs) || fiber::script::js_value_is_borrowed_binary(rhs)) {
                fiber::script::NativeBin lhs_bin = fiber::script::js_value_native_binary(lhs);
                fiber::script::NativeBin rhs_bin = fiber::script::js_value_native_binary(rhs);
                return lhs_bin.data == rhs_bin.data && lhs_bin.len == rhs_bin.len;
            }
            return fiber::script::js_value_heap_header(lhs) == fiber::script::js_value_heap_header(rhs);
        case fiber::script::JsNodeType::Array:
        case fiber::script::JsNodeType::Object:
        case fiber::script::JsNodeType::Interator:
        case fiber::script::JsNodeType::Exception:
            return fiber::script::js_value_heap_header(lhs) == fiber::script::js_value_heap_header(rhs);
        case fiber::script::JsNodeType::String:
            break;
    }
    return false;
}

bool loose_equal_number(double number, const fiber::script::JsValue &other) noexcept {
    if (is_number_type(fiber::script::js_value_type(other))) {
        return numbers_equal(number, number_value(other));
    }
    if (is_string_type(fiber::script::js_value_type(other))) {
        double other_number = 0.0;
        if (!string_to_number(other, other_number)) {
            return false;
        }
        return numbers_equal(number, other_number);
    }
    return false;
}

bool loose_equal(const fiber::script::JsValue &lhs, const fiber::script::JsValue &rhs) noexcept {
    if (is_string_type(fiber::script::js_value_type(lhs)) && is_string_type(fiber::script::js_value_type(rhs))) {
        int cmp = 0;
        if (!compare_strings(lhs, rhs, cmp)) {
            return false;
        }
        return cmp == 0;
    }
    if (is_number_type(fiber::script::js_value_type(lhs)) && is_number_type(fiber::script::js_value_type(rhs))) {
        return numbers_equal(number_value(lhs), number_value(rhs));
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::js_value_type(rhs)) {
        return strict_equal(lhs, rhs);
    }
    if ((fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Null &&
         fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Undefined) ||
        (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Undefined &&
         fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Null)) {
        return true;
    }
    if (fiber::script::js_value_type(lhs) == fiber::script::JsNodeType::Boolean) {
        double lhs_number = fiber::script::js_value_bool(lhs) ? 1.0 : 0.0;
        return loose_equal_number(lhs_number, rhs);
    }
    if (fiber::script::js_value_type(rhs) == fiber::script::JsNodeType::Boolean) {
        double rhs_number = fiber::script::js_value_bool(rhs) ? 1.0 : 0.0;
        return loose_equal_number(rhs_number, lhs);
    }
    if (is_number_type(fiber::script::js_value_type(lhs)) && is_string_type(fiber::script::js_value_type(rhs))) {
        double rhs_number = 0.0;
        if (!string_to_number(rhs, rhs_number)) {
            return false;
        }
        return numbers_equal(number_value(lhs), rhs_number);
    }
    if (is_string_type(fiber::script::js_value_type(lhs)) && is_number_type(fiber::script::js_value_type(rhs))) {
        double lhs_number = 0.0;
        if (!string_to_number(lhs, lhs_number)) {
            return false;
        }
        return numbers_equal(lhs_number, number_value(rhs));
    }
    return false;
}

bool equality(ConstValueHandle a, ConstValueHandle b, bool strict, bool invert) noexcept {
    bool equal = strict ? strict_equal(*a, *b) : loose_equal(*a, *b);
    return invert ? !equal : equal;
}

bool relation_result(double lhs, double rhs, bool less_direction, bool allow_equal) noexcept {
    if (std::isnan(lhs) || std::isnan(rhs)) {
        return false;
    }
    if (less_direction) {
        return allow_equal ? lhs <= rhs : lhs < rhs;
    }
    return allow_equal ? lhs >= rhs : lhs > rhs;
}

bool relation_result(int cmp, bool less_direction, bool allow_equal) noexcept {
    if (less_direction) {
        return allow_equal ? cmp <= 0 : cmp < 0;
    }
    return allow_equal ? cmp >= 0 : cmp > 0;
}

bool relation(ConstValueHandle a, ConstValueHandle b, bool less_direction, bool allow_equal) noexcept {
    if (is_string_type(fiber::script::js_value_type(*a)) && is_string_type(fiber::script::js_value_type(*b))) {
        int cmp = 0;
        if (!compare_strings(*a, *b, cmp)) {
            return false;
        }
        return relation_result(cmp, less_direction, allow_equal);
    }
    if (!is_numeric_like(fiber::script::js_value_type(*a)) || !is_numeric_like(fiber::script::js_value_type(*b))) {
        return false;
    }
    double lhs = 0.0;
    double rhs = 0.0;
    if (!to_number(*a, lhs) || !to_number(*b, rhs)) {
        return false;
    }
    return relation_result(lhs, rhs, less_direction, allow_equal);
}

} // namespace

bool Compares::neg(ConstValueHandle value) noexcept { return !logic(value); }

bool Compares::logic(ConstValueHandle value) noexcept {
    if (!value) {
        return false;
    }
    return is_truthy(*value);
}

bool Compares::eq(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, false, false); }

bool Compares::seq(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, true, false); }

bool Compares::ne(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, false, true); }

bool Compares::sne(ConstValueHandle a, ConstValueHandle b) noexcept { return equality(a, b, true, true); }

bool Compares::lt(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, true, false); }

bool Compares::lte(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, true, true); }

bool Compares::gt(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, false, false); }

bool Compares::gte(ConstValueHandle a, ConstValueHandle b) noexcept { return relation(a, b, false, true); }

bool Compares::matches(ConstValueHandle a, ConstValueHandle b) noexcept {
    (void) a;
    (void) b;
    return false;
}

bool Compares::in(ConstValueHandle a, ConstValueHandle b) noexcept {
    if (!a || !b) {
        return false;
    }
    if (fiber::script::js_value_type(*b) == fiber::script::JsNodeType::Array) {
        if (fiber::script::js_value_type(*a) != fiber::script::JsNodeType::Integer) {
            return false;
        }
        auto *arr = fiber::script::js_value_heap_ptr<const fiber::script::GcArray>(*b);
        std::int64_t index = fiber::script::js_value_int64(*a);
        if (!arr || index < 0) {
            return false;
        }
        return static_cast<std::size_t>(index) < arr->size;
    }
    if (fiber::script::js_value_type(*b) == fiber::script::JsNodeType::Object) {
        auto *obj = fiber::script::js_value_heap_ptr<const fiber::script::GcObject>(*b);
        if (!obj) {
            return false;
        }
        if (fiber::script::js_value_type(*a) == fiber::script::JsNodeType::String &&
            !fiber::script::js_value_is_borrowed_string(*a)) {
            auto *key_str = fiber::script::js_value_heap_ptr<const fiber::script::GcString>(*a);
            const fiber::script::JsValue *found = fiber::script::gc_object_get(obj, key_str);
            return found != nullptr;
        }
        if (fiber::script::js_value_type(*a) == fiber::script::JsNodeType::String &&
            fiber::script::js_value_is_borrowed_string(*a)) {
            fiber::script::NativeStr native = fiber::script::js_value_native_string(*a);
            for (const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_first_entry(obj);
                 entry != nullptr; entry = fiber::script::gc_object_next_entry(obj, entry)) {
                if (!entry->occupied || !entry->key) {
                    continue;
                }
                if (heap_string_equals_native_utf8(entry->key, native)) {
                    return true;
                }
            }
        }
        return false;
    }
    return false;
}

} // namespace fiber::script::run
