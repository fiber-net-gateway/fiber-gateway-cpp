#include "script/std/BinaryFuncs.h"

#include <fiber/script/std/NodeText.h>
#include <fiber/script/std/StdLibrary.h>

#include <fiber/common/util/Base64.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/std/OomGuard.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::script::std_lib {

namespace {

AbiResult type_error() noexcept { return AbiResult::exception(JsValue::make_exception(ExceptionKind::TypeError)); }

AbiResult range_error() noexcept { return AbiResult::exception(JsValue::make_exception(ExceptionKind::RangeError)); }

JsValue empty_string() noexcept { return JsValue::make_native_string("", 0); }

// Builds a heap string result (empty -> borrowed empty string), mapping make_string's
// OOM (returns undefined) to a graceful abort.
AbiResult make_string_result(GcHeap *heap, std::string_view sv) noexcept {
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    if (sv.empty()) {
        return AbiResult::success(empty_string());
    }
    JsValue result = JsValue::make_string(*heap, sv.data(), sv.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// Builds a heap binary result. make_binary returns undefined on OOM (and otherwise handles
// len == 0 without dereferencing data).
AbiResult make_binary_result(GcHeap *heap, const std::uint8_t *data, std::size_t len) noexcept {
    if (heap == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    JsValue result = JsValue::make_binary(*heap, data, len);
    if (js_value_type(result) != JsNodeType::Binary) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// Hex digit value 0-15, or -1 for non-hex.
constexpr int hex_digit(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// ---- binary.base64Encode: Binary -> base64 String; non-binary -> undefined ----
// Huffman-style two-phase (cf. src/http/Huffman.h): exact size precompute, one GC
// allocation, direct fill. No throwing std::string staging, so noexcept holds; OOM
// maps to a graceful abort.

AbiResult base64_encode_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // Java MissingNode -> undefined.
        return AbiResult::success(JsValue::make_undefined());
    }
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;
    if (!binary_bytes(args.args[0], data, len)) {
        return AbiResult::success(JsValue::make_undefined());
    }
    if (len == 0) {
        return AbiResult::success(empty_string());
    }
    GcHeap &heap = frame.runtime;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    if (!out) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    GcHeap::NoGcScope no_gc(heap);
    const std::size_t enc_len = fiber::util::base64_encoded_size(len);
    // Base64 output is pure ASCII: utf16 length == byte length, always well-formed.
    GcString *str = gc_new_string_wtf8_uninit(&heap, enc_len, enc_len, true);
    if (!str) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    fiber::util::base64_encode_into(data, len, gc_string_wtf8_data(str));
    GcString *interned = gc_detail::gc_string_intern_final(&heap, str);
    *out = js_make_heap_ref(&interned->hdr, GcHeapKind::String);
    return AbiResult::success(*out);
}

// ---- binary.base64Decode: String -> Binary; non-string -> undefined; invalid -> RangeError ----
// Two-phase like encode: base64_decoded_size validates structure and yields the
// exact length; gc_new_binary_uninit allocates; base64_decode_into fills in place.

AbiResult base64_decode_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return AbiResult::success(JsValue::make_undefined());
    }
    std::string_view text;
    if (!string_utf8_view(args.args[0], text)) {
        return AbiResult::success(JsValue::make_undefined());
    }
    bool ok = false;
    const std::size_t dec_len = fiber::util::base64_decoded_size(text, ok);
    if (!ok) {
        // Java Base64.getDecoder().decode throws IllegalArgumentException -> RangeError.
        return range_error();
    }
    if (dec_len == 0) {
        return make_binary_result(&frame.runtime, nullptr, 0);
    }
    GcHeap &heap = frame.runtime;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    if (!out) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    GcHeap::NoGcScope no_gc(heap);
    GcBinary *bin = gc_new_binary_uninit(&heap, dec_len);
    if (!bin) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!fiber::util::base64_decode_into(text, bin->data)) {
        // Alphabet-invalid; structure was already validated by decoded_size.
        return range_error();
    }
    *out = js_make_heap_ref(&bin->hdr, GcHeapKind::Binary);
    return AbiResult::success(*out);
}

// ---- binary.hex: Binary -> lowercase hex String; non-binary -> TypeError ----
// Huffman-style two-phase like base64_encode_fn: exact size (2n), one GC string,
// hex_encode writes in place. Hex output is pure ASCII: utf16 length == byte
// length, always well-formed.

AbiResult hex_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;
    if (!binary_bytes(args.args[0], data, len)) {
        // Java throws ScriptExecException -> TypeError.
        return type_error();
    }
    if (len == 0) {
        return AbiResult::success(empty_string());
    }
    GcHeap &heap = frame.runtime;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    if (!out) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    GcHeap::NoGcScope no_gc(heap);
    const std::size_t hex_len = len * 2;
    GcString *str = gc_new_string_wtf8_uninit(&heap, hex_len, hex_len, true);
    if (!str) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    hex_encode(data, len, gc_string_wtf8_data(str));
    GcString *interned = gc_detail::gc_string_intern_final(&heap, str);
    *out = js_make_heap_ref(&interned->hdr, GcHeapKind::String);
    return AbiResult::success(*out);
}

// ---- binary.fromHex: String -> Binary; non-string -> TypeError; invalid hex -> RangeError ----
// Two-phase like base64_decode_fn: gc_new_binary_uninit allocates, the digit
// loop fills in place (invalid digit -> RangeError, same semantics as before).

AbiResult from_hex_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    std::string_view text;
    if (!string_utf8_view(args.args[0], text)) {
        return type_error();
    }
    if (text.size() % 2 != 0) {
        // Strict: Java's lenient truncation is intentionally not mirrored.
        return range_error();
    }
    if (text.empty()) {
        return make_binary_result(&frame.runtime, nullptr, 0);
    }
    GcHeap &heap = frame.runtime;
    GcHeap::LocalMark mark(heap);
    ValueHandle out = heap.local_value();
    if (!out) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    GcHeap::NoGcScope no_gc(heap);
    GcBinary *bin = gc_new_binary_uninit(&heap, text.size() / 2);
    if (!bin) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = hex_digit(static_cast<unsigned char>(text[i]));
        const int lo = hex_digit(static_cast<unsigned char>(text[i + 1]));
        if (hi < 0 || lo < 0) {
            return range_error();
        }
        bin->data[i / 2] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    *out = js_make_heap_ref(&bin->hdr, GcHeapKind::Binary);
    return AbiResult::success(*out);
}

// ---- binary.getUtf8Bytes: any -> Binary = JsonUtil.toString(value) UTF-8 bytes ----

AbiResult get_utf8_bytes_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments args) noexcept {
    // Staging string throws on OOM; the guard maps it to a graceful abort.
    return oom_guard([&] {
        std::string out;
        if (args.args && args.argc >= 1) {
            node_json_to_string(args.args[0], out);
        }
        return make_binary_result(&frame.runtime, reinterpret_cast<const std::uint8_t *>(out.data()), out.size());
    });
}

} // namespace

void register_binary_funcs(StdLibrary &lib) {
    lib.register_func("binary.base64Encode",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &base64_encode_fn, nullptr, "binary.base64Encode");
    lib.register_func("binary.base64Decode",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &base64_decode_fn, nullptr, "binary.base64Decode");
    lib.register_func("binary.hex", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &hex_fn, nullptr, "binary.hex");
    lib.register_func("binary.fromHex",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &from_hex_fn,
                      nullptr, "binary.fromHex");
    lib.register_func("binary.getUtf8Bytes",
                      Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &get_utf8_bytes_fn, nullptr, "binary.getUtf8Bytes");
}

} // namespace fiber::script::std_lib
