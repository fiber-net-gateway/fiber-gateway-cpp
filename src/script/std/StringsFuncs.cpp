#include "StringsFuncs.h"

#include "StdLibrary.h"

#include "NodeText.h"

#include "../../common/json/Utf.h"
#include "../JsValue.h"
#include "../Library.h"
#include "../gc/GcInternal.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace fiber::script::std_lib {

namespace {

// NOTE: strings.match and strings.findAll (regex) are deliberately absent. They
// depend on a regex-engine decision that is still open; add them alongside a
// regex implementation when that choice is made.

JsValue empty_string() noexcept { return JsValue::make_native_string("", 0); }

// Builds a heap string result from a view (empty -> borrowed empty string),
// mapping make_string's OOM (returns undefined) to a graceful abort.
ScriptResult make_string_result(GcHeap *heap, std::string_view sv) noexcept {
    if (heap == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    if (sv.empty()) {
        return ScriptResult::success(empty_string());
    }
    JsValue result = JsValue::make_string(*heap, sv.data(), sv.size());
    if (js_value_type(result) != JsNodeType::String) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return ScriptResult::success(result);
}

// Character.isWhitespace ASCII subset: {0x09-0x0D, 0x1C-0x20}. Exotic Unicode
// whitespace (U+0085 NEL, U+2000-U+3000, ...) is NOT trimmed, diverging from
// Java; the multi-byte whitespace chars cannot be matched byte-wise here.
bool is_java_ws_ascii(unsigned char b) noexcept {
    switch (b) {
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x1C:
        case 0x1D:
        case 0x1E:
        case 0x1F:
        case 0x20:
            return true;
        default:
            return false;
    }
}

// Mirrors Jackson's JsonNode.asInt(): Integer->value, Float->trunc-toward-zero
// (NaN->0, +/-Inf->INT32_MAX/MIN like Java's (int)double), Boolean->0/1,
// Null/Undefined->0, String->leading integer parse else 0, others->0. Used by
// strings.substring's start/end (which default to 0 / INT32_MAX).
std::int64_t as_int(const JsValue &v) noexcept {
    switch (js_value_type(v)) {
        case JsNodeType::Integer:
            return js_value_int64(v);
        case JsNodeType::Float: {
            double d = js_value_double(v);
            if (std::isnan(d)) {
                return 0;
            }
            if (std::isinf(d)) {
                return d > 0 ? static_cast<std::int64_t>(INT32_MAX) : static_cast<std::int64_t>(INT32_MIN);
            }
            return static_cast<std::int64_t>(d); // truncate toward zero
        }
        case JsNodeType::Boolean:
            return js_value_bool(v) ? 1 : 0;
        case JsNodeType::Null:
        case JsNodeType::Undefined:
            return 0;
        case JsNodeType::String: {
            std::string_view sv;
            if (string_utf8_view(v, sv)) {
                std::int64_t val = 0;
                auto r = std::from_chars(sv.data(), sv.data() + sv.size(), val);
                if (r.ec == std::errc{}) {
                    return val;
                }
            }
            return 0;
        }
        default:
            return 0;
    }
}

// UTF-16 code-unit length of a UTF-8 view (Java String.length()). Falls back to
// the byte count when the view is not valid UTF-8 (malformed borrowed input).
std::size_t utf16_len(std::string_view v) noexcept {
    fiber::json::Utf8ScanResult scan;
    if (fiber::json::utf8_scan(v.data(), v.size(), scan)) {
        return scan.utf16_len;
    }
    return v.size();
}

// Byte offset of the codepoint whose UTF-16 start index is the largest value
// <= u16idx (i.e. the codepoint containing the u16idx-th unit). Used by
// substring to map UTF-16 indices back to UTF-8 byte offsets; only called with
// u16idx < total UTF-16 length.
std::size_t u16_index_to_byte(std::string_view v, std::size_t u16idx) noexcept {
    std::size_t pos = 0;
    std::size_t u16pos = 0;
    std::size_t best = 0;
    std::uint32_t cp = 0;
    while (pos < v.size()) {
        if (u16pos > u16idx) {
            break;
        }
        best = pos;
        if (!fiber::json::utf8_next_codepoint(v.data(), v.size(), pos, cp)) {
            break;
        }
        u16pos += (cp <= 0xFFFF) ? 1 : 2;
    }
    return best;
}

// UTF-16 index of the codepoint starting at byte offset byte_off.
std::size_t byte_to_u16_index(std::string_view v, std::size_t byte_off) noexcept {
    std::size_t pos = 0;
    std::size_t u16pos = 0;
    std::uint32_t cp = 0;
    while (pos < byte_off && pos < v.size()) {
        if (!fiber::json::utf8_next_codepoint(v.data(), v.size(), pos, cp)) {
            break;
        }
        u16pos += (cp <= 0xFFFF) ? 1 : 2;
    }
    return u16pos;
}

// True if the codepoint set of `set` contains `target`. Walks `set` each call;
// for the small cutsets typical of contains_any/indexAny this is fine and keeps
// the *any funcs allocation-free.
bool codepoint_set_contains(std::string_view set, std::uint32_t target) noexcept {
    std::size_t pos = 0;
    std::uint32_t cp = 0;
    while (pos < set.size() && fiber::json::utf8_next_codepoint(set.data(), set.size(), pos, cp)) {
        if (cp == target) {
            return true;
        }
    }
    return false;
}

// Last UTF-16 index in `text` whose codepoint equals `cp`, or SIZE_MAX if none.
std::size_t last_u16_index_of_cp(std::string_view text, std::uint32_t cp) noexcept {
    std::size_t pos = 0;
    std::size_t u16pos = 0;
    std::size_t last = static_cast<std::size_t>(-1);
    std::uint32_t c = 0;
    while (pos < text.size() && fiber::json::utf8_next_codepoint(text.data(), text.size(), pos, c)) {
        if (c == cp) {
            last = u16pos;
        }
        u16pos += (c <= 0xFFFF) ? 1 : 2;
    }
    return last;
}

// ---- strings.hasPrefix(text, prefix) ----

ScriptResult has_prefix_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                           Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view prefix;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], prefix)) {
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    return ScriptResult::success(JsValue::make_boolean(text.starts_with(prefix)));
}

// ---- strings.hasSuffix(text, suffix) ----

ScriptResult has_suffix_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                           Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view suffix;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], suffix)) {
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    return ScriptResult::success(JsValue::make_boolean(text.ends_with(suffix)));
}

// ---- strings.toLower(text) / strings.toUpper(text) ----
// ASCII fast path only: A-Z<->a-z are flipped, all other bytes (including
// multi-byte UTF-8 sequences) are copied verbatim, preserving UTF-8 structure.
// Non-ASCII case mapping diverges from Java's locale-aware toLowerCase/UpperCase.

ScriptResult to_lower_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::string out;
    out.reserve(text.size());
    for (unsigned char b: text) {
        out.push_back((b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : static_cast<char>(b));
    }
    return make_string_result(&frame.runtime, std::string_view(out));
}

ScriptResult to_upper_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::string out;
    out.reserve(text.size());
    for (unsigned char b: text) {
        out.push_back((b >= 'a' && b <= 'z') ? static_cast<char>(b - 32) : static_cast<char>(b));
    }
    return make_string_result(&frame.runtime, std::string_view(out));
}

// ---- strings.trim(text, cutset=null) ----
// cutset null -> Java String.trim(): strip code points <= 0x20 from both ends
//   (all such code points are single-byte in UTF-8).
// cutset textual -> StringUtils.trim: repeatedly strip the cutset SUBSTRING
//   (regionMatches) from both ends; empty src/cutset returns src unchanged.

ScriptResult trim_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::string_view cutset;
    std::string_view result;
    if (args.argc < 2 || !string_utf8_view(args.args[1], cutset)) {
        // Java String.trim(): strip code points <= 0x20.
        std::size_t s = 0;
        std::size_t e = text.size();
        while (s < e && static_cast<unsigned char>(text[s]) <= 0x20) {
            ++s;
        }
        while (e > s && static_cast<unsigned char>(text[e - 1]) <= 0x20) {
            --e;
        }
        result = text.substr(s, e - s);
    } else if (text.empty() || cutset.empty()) {
        result = text; // StringUtils.trim isEmpty guard returns src.
    } else {
        long long oLen = static_cast<long long>(text.size());
        long long len = static_cast<long long>(cutset.size());
        long long s = 0;
        long long e = oLen - len;
        while (s < oLen && text.substr(static_cast<std::size_t>(s), static_cast<std::size_t>(len)) == cutset) {
            s += len;
        }
        while (e >= s && text.substr(static_cast<std::size_t>(e), static_cast<std::size_t>(len)) == cutset) {
            e -= len;
        }
        e += len;
        if (s >= e) {
            return ScriptResult::success(empty_string());
        }
        result = text.substr(static_cast<std::size_t>(s), static_cast<std::size_t>(e - s));
    }
    return make_string_result(&frame.runtime, result);
}

// ---- strings.trimLeft(text, cutset=null) ----
// cutset null -> StringUtils.trimLeftEmpty (Character.isWhitespace, ASCII subset).
// cutset textual -> StringUtils.trimLeft (strip cutset substring from left).

ScriptResult trim_left_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::string_view cutset;
    std::string_view result;
    if (args.argc < 2 || !string_utf8_view(args.args[1], cutset)) {
        std::size_t s = 0;
        while (s < text.size() && is_java_ws_ascii(static_cast<unsigned char>(text[s]))) {
            ++s;
        }
        result = text.substr(s);
    } else if (text.empty() || cutset.empty()) {
        result = text;
    } else {
        long long oLen = static_cast<long long>(text.size());
        long long len = static_cast<long long>(cutset.size());
        long long s = 0;
        while (s < oLen && text.substr(static_cast<std::size_t>(s), static_cast<std::size_t>(len)) == cutset) {
            s += len;
        }
        result = text.substr(static_cast<std::size_t>(s));
    }
    return make_string_result(&frame.runtime, result);
}

// ---- strings.trimRight(text, cutset=null) ----

ScriptResult trim_right_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::string_view cutset;
    std::string_view result;
    if (args.argc < 2 || !string_utf8_view(args.args[1], cutset)) {
        std::size_t e = text.size();
        while (e > 0 && is_java_ws_ascii(static_cast<unsigned char>(text[e - 1]))) {
            --e;
        }
        result = text.substr(0, e);
    } else if (text.empty() || cutset.empty()) {
        result = text;
    } else {
        long long oLen = static_cast<long long>(text.size());
        long long len = static_cast<long long>(cutset.size());
        long long e = oLen - len;
        while (e >= 0 && text.substr(static_cast<std::size_t>(e), static_cast<std::size_t>(len)) == cutset) {
            e -= len;
        }
        e += len;
        if (e <= 0) {
            return ScriptResult::success(empty_string());
        }
        result = text.substr(0, static_cast<std::size_t>(e));
    }
    return make_string_result(&frame.runtime, result);
}

// ---- strings.split(text, separator=null) ----
// separator null -> [text]. separator textual -> StringUtils.split: the
// separator is a SET of code points (any one splits), adjacent separators
// collapse (no empty tokens), trailing separators produce no token, empty text
// -> []. Result array grows via gc_array_push (GC-safe, internally rooted); the
// source view stays valid because args are rooted for the host call's duration.

ScriptResult split_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    GcHeap *heap = &frame.runtime;
    if (heap == nullptr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }

    std::string_view sep;
    if (args.argc < 2 || !string_utf8_view(args.args[1], sep)) {
        // No separator: return [text] (the original value, matching Java identity).
        GcHeap::LocalMark mark(*heap);
        ValueHandle arr = heap->local_value();
        if (!arr) {
            return ScriptResult::abort(ScriptAbortReason::InvalidState);
        }
        *arr = JsValue::make_array(*heap, 1);
        if (js_value_type(*arr) != JsNodeType::Array) {
            return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
        }
        if (!gc_array_push(heap, arr, args.args[0])) {
            return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
        }
        return ScriptResult::success(*arr);
    }

    GcHeap::LocalMark mark(*heap);
    ValueHandle arr = heap->local_value();
    if (!arr) {
        return ScriptResult::abort(ScriptAbortReason::InvalidState);
    }
    *arr = JsValue::make_array(*heap, 0);
    if (js_value_type(*arr) != JsNodeType::Array) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }

    auto emit = [&](std::size_t start, std::size_t len) -> bool {
        JsValue item = JsValue::make_string(*heap, text.data() + start, len);
        if (js_value_type(item) != JsNodeType::String) {
            return false;
        }
        return gc_array_push(heap, arr, item);
    };

    std::size_t pos = 0;
    std::size_t start = 0;
    bool match = false;
    std::uint32_t cp = 0;
    while (pos < text.size()) {
        std::size_t cp_start = pos;
        if (!fiber::json::utf8_next_codepoint(text.data(), text.size(), pos, cp)) {
            // Malformed byte (borrowed input): treat as token content, skip one byte.
            match = true;
            pos = cp_start + 1;
            continue;
        }
        if (codepoint_set_contains(sep, cp)) {
            if (match) {
                if (!emit(start, cp_start - start)) {
                    return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
                }
                match = false;
            }
            start = pos; // next token begins after this separator codepoint
        } else {
            match = true;
        }
    }
    if (match) {
        if (!emit(start, text.size() - start)) {
            return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
        }
    }
    return ScriptResult::success(*arr);
}

// ---- strings.contains(text, value) ----

ScriptResult contains_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                         Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view value;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], value)) {
        return ScriptResult::success(JsValue::make_null());
    }
    return ScriptResult::success(JsValue::make_boolean(text.find(value) != std::string_view::npos));
}

// ---- strings.contains_any(text, chars) ----

ScriptResult contains_any_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                             Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view chars;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], chars)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::size_t pos = 0;
    std::uint32_t cp = 0;
    while (pos < text.size() && fiber::json::utf8_next_codepoint(text.data(), text.size(), pos, cp)) {
        if (codepoint_set_contains(chars, cp)) {
            return ScriptResult::success(JsValue::make_boolean(true));
        }
    }
    return ScriptResult::success(JsValue::make_boolean(false));
}

// ---- strings.index(text, value) ----

ScriptResult index_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/, Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view value;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], value)) {
        return ScriptResult::success(JsValue::make_null());
    }
    auto p = text.find(value);
    if (p == std::string_view::npos) {
        return ScriptResult::success(JsValue::make_integer(-1));
    }
    return ScriptResult::success(JsValue::make_integer(static_cast<std::int64_t>(byte_to_u16_index(text, p))));
}

// ---- strings.indexAny(text, chars) ----

ScriptResult index_any_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                          Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view chars;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], chars)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::size_t pos = 0;
    std::size_t u16pos = 0;
    std::uint32_t cp = 0;
    while (pos < text.size() && fiber::json::utf8_next_codepoint(text.data(), text.size(), pos, cp)) {
        if (codepoint_set_contains(chars, cp)) {
            return ScriptResult::success(JsValue::make_integer(static_cast<std::int64_t>(u16pos)));
        }
        u16pos += (cp <= 0xFFFF) ? 1 : 2;
    }
    return ScriptResult::success(JsValue::make_integer(-1));
}

// ---- strings.lastIndex(text, value) ----

ScriptResult last_index_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                           Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view value;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], value)) {
        return ScriptResult::success(JsValue::make_null());
    }
    auto p = text.rfind(value);
    if (p == std::string_view::npos) {
        return ScriptResult::success(JsValue::make_integer(-1));
    }
    return ScriptResult::success(JsValue::make_integer(static_cast<std::int64_t>(byte_to_u16_index(text, p))));
}

// ---- strings.lastIndexAny(text, chars) ----
// Mirrors Java: iterate the cutset code points IN ORDER; return the last
// occurrence (UTF-16 index) of the FIRST cutset code point that appears, else -1.
// (Not the global maximum index.)

ScriptResult last_index_any_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                               Library::Arguments args) noexcept {
    std::string_view text;
    std::string_view chars;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text) ||
        !string_utf8_view(args.args[1], chars)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::size_t pos = 0;
    std::uint32_t cp = 0;
    while (pos < chars.size() && fiber::json::utf8_next_codepoint(chars.data(), chars.size(), pos, cp)) {
        std::size_t li = last_u16_index_of_cp(text, cp);
        if (li != static_cast<std::size_t>(-1)) {
            return ScriptResult::success(JsValue::make_integer(static_cast<std::int64_t>(li)));
        }
    }
    return ScriptResult::success(JsValue::make_integer(-1));
}

// ---- strings.repeat(text, count) ----

// Cap total output to avoid pathological repeat counts dragging the process
// down (and to stay clear of std::string's throwing reserve under OOM). 16 MiB.
constexpr std::uint64_t kMaxRepeatBytes = 1ull << 24;

ScriptResult repeat_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 2 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    const JsValue &count_arg = args.args[1];
    JsNodeType ct = js_value_type(count_arg);
    std::int32_t i;
    if (ct == JsNodeType::Integer) {
        i = static_cast<std::int32_t>(js_value_int64(count_arg)); // mirror Java intValue() narrowing
    } else if (ct == JsNodeType::Float) {
        double d = js_value_double(count_arg);
        if (std::isnan(d)) {
            i = 0;
        } else if (std::isinf(d)) {
            i = d > 0 ? INT32_MAX : INT32_MIN;
        } else {
            i = static_cast<std::int32_t>(static_cast<std::int64_t>(d)); // truncate toward zero, narrow
        }
    } else {
        return ScriptResult::success(JsValue::make_null());
    }
    if (i < 0) {
        return ScriptResult::success(JsValue::make_null());
    }
    if (i == 0 || text.empty()) {
        return ScriptResult::success(empty_string());
    }
    if (i == 1) {
        return ScriptResult::success(args.args[0]); // Java returns the original text node
    }
    std::size_t len = text.size();
    if (static_cast<std::uint64_t>(i) * len > kMaxRepeatBytes) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(i) * len);
    for (std::int32_t j = 0; j < i; ++j) {
        out.append(text.data(), len);
    }
    return make_string_result(&frame.runtime, std::string_view(out));
}

// ---- strings.substring(text, start=0, end=2147483647) ----
// start/end are UTF-16 code-unit indices (matching Java String and length()).

ScriptResult substring_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    std::string_view text;
    if (!args.args || args.argc < 1 || !string_utf8_view(args.args[0], text)) {
        return ScriptResult::success(JsValue::make_null());
    }
    std::size_t u16len = utf16_len(text);
    std::int64_t i = (args.argc >= 2) ? as_int(args.args[1]) : 0; // start, default 0
    if (i >= static_cast<std::int64_t>(u16len)) {
        return ScriptResult::success(empty_string());
    }
    if (i < 0) {
        i = 0;
    }
    std::int64_t j = (args.argc >= 3) ? as_int(args.args[2]) : 2147483647; // end, default INT32_MAX
    if (j <= i) {
        return ScriptResult::success(empty_string());
    }
    if (j >= static_cast<std::int64_t>(u16len)) {
        if (i == 0) {
            return ScriptResult::success(args.args[0]); // Java returns the original text node
        }
        std::size_t sb = u16_index_to_byte(text, static_cast<std::size_t>(i));
        return make_string_result(&frame.runtime, text.substr(sb));
    }
    std::size_t sb = u16_index_to_byte(text, static_cast<std::size_t>(i));
    std::size_t eb = u16_index_to_byte(text, static_cast<std::size_t>(j));
    return make_string_result(&frame.runtime, text.substr(sb, eb - sb));
}

// ---- strings.toString() ----

ScriptResult to_string0_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                           Library::Arguments /*args*/) noexcept {
    return ScriptResult::success(empty_string());
}

// ---- strings.toString(value) ----
// null/undefined -> "null"; otherwise JsonUtil.toString, which NodeText.h's
// node_json_to_string mirrors (Array -> "<ArrayNode>", Object -> "<ObjectNode>",
// Binary -> raw bytes, scalars -> asText).

ScriptResult to_string1_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return ScriptResult::success(empty_string());
    }
    const JsValue &v = args.args[0];
    JsNodeType t = js_value_type(v);
    if (t == JsNodeType::Null || t == JsNodeType::Undefined) {
        return make_string_result(&frame.runtime, std::string_view("null", 4));
    }
    std::string out;
    node_json_to_string(v, out);
    return make_string_result(&frame.runtime, std::string_view(out));
}

} // namespace

void register_strings_funcs(StdLibrary &lib) {
    lib.register_func("strings.hasPrefix",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                      &has_prefix_fn, nullptr, "strings.hasPrefix");
    lib.register_func("strings.hasSuffix",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                      &has_suffix_fn, nullptr, "strings.hasSuffix");
    lib.register_func("strings.toLower",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &to_lower_fn,
                      nullptr, "strings.toLower");
    lib.register_func("strings.toUpper",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &to_upper_fn,
                      nullptr, "strings.toUpper");
    lib.register_func("strings.trim",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 2, .variadic = false},
                      {JsValue::make_null()}, &trim_fn, nullptr, "strings.trim");
    lib.register_func("strings.trimLeft",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 2, .variadic = false},
                      {JsValue::make_null()}, &trim_left_fn, nullptr, "strings.trimLeft");
    lib.register_func("strings.trimRight",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 2, .variadic = false},
                      {JsValue::make_null()}, &trim_right_fn, nullptr, "strings.trimRight");
    lib.register_func("strings.split",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 2, .variadic = false},
                      {JsValue::make_null()}, &split_fn, nullptr, "strings.split");
    // strings.findAll / strings.match: deferred (regex engine TBD).
    lib.register_func("strings.contains",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &contains_fn,
                      nullptr, "strings.contains");
    lib.register_func("strings.contains_any",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                      &contains_any_fn, nullptr, "strings.contains_any");
    lib.register_func("strings.index",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &index_fn,
                      nullptr, "strings.index");
    lib.register_func("strings.indexAny",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &index_any_fn,
                      nullptr, "strings.indexAny");
    lib.register_func("strings.lastIndex",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                      &last_index_fn, nullptr, "strings.lastIndex");
    lib.register_func("strings.lastIndexAny",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false},
                      &last_index_any_fn, nullptr, "strings.lastIndexAny");
    lib.register_func("strings.repeat",
                      Library::FunctionSignature{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &repeat_fn,
                      nullptr, "strings.repeat");
    lib.register_func(
            "strings.substring", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 3, .variadic = false},
            {JsValue::make_integer(0), JsValue::make_integer(2147483647)}, &substring_fn, nullptr, "strings.substring");
    lib.register_func("strings.toString",
                      Library::FunctionSignature{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                      &to_string0_fn, nullptr, "strings.toString");
    lib.register_func("strings.toString",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &to_string1_fn, nullptr, "strings.toString");
}

} // namespace fiber::script::std_lib
