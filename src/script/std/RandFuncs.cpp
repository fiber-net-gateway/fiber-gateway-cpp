#include "RandFuncs.h"

#include "StdLibrary.h"

#include "../JsValue.h"
#include "../Library.h"
#include "../gc/GcInternal.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <system_error>

namespace fiber::script::std_lib {

namespace {

// ---- catchable errors (mirror Java's ScriptExecException / IllegalArgumentException) ----

ScriptResult type_error() noexcept {
    return ScriptResult::exception(JsValue::make_exception(ExceptionKind::TypeError));
}

ScriptResult range_error() noexcept {
    return ScriptResult::exception(JsValue::make_exception(ExceptionKind::RangeError));
}

// ---- numeric coercion helpers ----

// Java's (long)double saturates at int64 range; a plain static_cast is undefined behavior
// for out-of-range magnitudes. This mirrors the saturation (and (long)NaN == 0) so the
// bound extraction stays UB-free and parity-faithful.
std::int64_t double_to_int64_saturated(double d) noexcept {
    if (std::isnan(d)) {
        return 0;
    }
    if (d >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (d < static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(d);
}

// ---- per-thread PRNG (mirrors Java's ThreadLocalRandom) ----

std::uint64_t next_u64() {
    // thread_local seeds once per worker thread from the OS entropy source, matching
    // ThreadLocalRandom's per-thread, non-deterministic seeding. mt19937_64 holds its
    // state inline (no dynamic allocation), so thread_local is safe under the loop-per-
    // thread model.
    thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine();
}

// Uniform unbiased sample in [0, bound) over the 64-bit space, matching
// ThreadLocalRandom.nextLong(bound) (rejection sampling, not plain modulo). The caller
// guarantees bound > 0.
std::int64_t next_bounded(std::uint64_t bound) noexcept {
    // Smallest power-of-two mask >= bound, then reject samples that land outside [0, bound).
    std::uint64_t mask = bound - 1;
    mask |= mask >> 1;
    mask |= mask >> 2;
    mask |= mask >> 4;
    mask |= mask >> 8;
    mask |= mask >> 16;
    mask |= mask >> 32;
    std::uint64_t sample;
    do {
        sample = next_u64() & mask;
    } while (sample >= bound);
    return static_cast<std::int64_t>(sample);
}

// ---- CRC-32 (java.util.zip.CRC32 parity) ----
// Reflected poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF: the standard zlib/
// Ethernet CRC-32 that java.util.zip.CRC32 implements (empty input -> 0, the canonical
// check value "123456789" -> 0xCBF43926). Built once at compile time.

constexpr std::uint32_t kCrc32Poly = 0xEDB88320u;

constexpr std::uint32_t crc32_table_entry(std::uint32_t index) noexcept {
    std::uint32_t c = index;
    for (int k = 0; k < 8; ++k) {
        c = (c & 1u) != 0u ? (kCrc32Poly ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

struct Crc32Table {
    std::uint32_t entries[256]{};
    constexpr Crc32Table() noexcept {
        for (std::uint32_t i = 0; i < 256u; ++i) {
            entries[i] = crc32_table_entry(i);
        }
    }
};
constexpr Crc32Table kCrc32Table;

void crc32_update(std::uint32_t &crc, const char *data, std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        std::uint32_t idx = (crc ^ static_cast<std::uint8_t>(data[i])) & 0xFFu;
        crc = kCrc32Table.entries[idx] ^ (crc >> 8);
    }
}

// ---- rand.canary key text (mirrors Jackson's JsonNode.asText(), no-arg) ----
// Differs from asText("") on exactly one point: null renders to "null" rather than "".
// Undefined/containers/binaries render to "" and are skipped by the caller's empty check.
void canary_key_text(const JsValue &value, std::string &out) {
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
                std::string tmp;
                if (gc_string_to_utf8(str, tmp) && !tmp.empty()) {
                    out.append(tmp);
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

// ---- rand.random(max) ----

ScriptResult random_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                       Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    const JsValue &max = args.args[0];

    // Java requires max.isNumber(); integers pass through, floats truncate toward zero
    // (with saturation) like Jackson's longValue() on a DoubleNode.
    std::int64_t bound;
    switch (js_value_type(max)) {
        case JsNodeType::Integer:
            bound = js_value_int64(max);
            break;
        case JsNodeType::Float:
            bound = double_to_int64_saturated(js_value_double(max));
            break;
        default:
            return type_error();
    }

    // Java's ThreadLocalRandom.nextLong(bound) throws IllegalArgumentException for bound <= 0.
    if (bound <= 0) {
        return range_error();
    }
    return ScriptResult::success(JsValue::make_integer(next_bounded(static_cast<std::uint64_t>(bound))));
}

// ---- rand.canary(ratio, ...keys) ----

ScriptResult canary_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/,
                       Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // ratio is required; the resolver rejects arity < 1, but guard defensively.
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    const JsValue &ratio_val = args.args[0];

    // Jackson's asLong(0L): non-numeric collapses to 0 (never throws). Mirror that lenient
    // extraction so a non-numeric ratio simply evaluates to false.
    std::int64_t ratio = 0;
    switch (js_value_type(ratio_val)) {
        case JsNodeType::Integer:
            ratio = js_value_int64(ratio_val);
            break;
        case JsNodeType::Float:
            ratio = double_to_int64_saturated(js_value_double(ratio_val));
            break;
        case JsNodeType::Boolean:
            ratio = js_value_bool(ratio_val) ? 1 : 0;
            break;
        default:
            ratio = 0;
            break;
    }

    if (ratio <= 0) {
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    if (ratio >= 100) {
        return ScriptResult::success(JsValue::make_boolean(true));
    }

    // No keys -> random bucket, as in Java (non-deterministic).
    if (args.argc == 1) {
        bool hit = static_cast<std::int64_t>(next_bounded(100)) < ratio;
        return ScriptResult::success(JsValue::make_boolean(hit));
    }

    // With keys -> deterministic bucket from CRC-32 over the non-empty key texts. A single
    // CRC instance is updated once per key in order (cumulative), matching Java.
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint32_t i = 1; i < args.argc; ++i) {
        std::string text;
        canary_key_text(args.args[i], text);
        if (text.empty()) {
            continue;
        }
        crc32_update(crc, text.data(), text.size());
    }
    crc ^= 0xFFFFFFFFu;
    bool hit = (static_cast<std::uint64_t>(crc) % 100u) < static_cast<std::uint64_t>(ratio);
    return ScriptResult::success(JsValue::make_boolean(hit));
}

} // namespace

void register_rand_funcs(StdLibrary &lib) {
    // max is optional with default 1000 (matches Java's @ScriptParam defaultValue = "1000").
    lib.register_func("rand.random", Library::FunctionSignature{.required_argc = 0, .fixed_argc = 1, .variadic = false},
                      {JsValue::make_integer(1000)}, &random_fn, nullptr, "rand.random");
    // ratio is required; keys are variadic.
    lib.register_func("rand.canary", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = true},
                      &canary_fn, nullptr, "rand.canary");
}

} // namespace fiber::script::std_lib
