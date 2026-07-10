#include "HashFuncs.h"

#include "Crc32.h"
#include "NodeText.h"
#include "StdLibrary.h"

#include "../JsValue.h"
#include "../Library.h"
#include "../gc/GcInternal.h"

#include <openssl/evp.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::script::std_lib {

namespace {

ScriptResult type_error() noexcept {
    return ScriptResult::exception(JsValue::make_exception(ExceptionKind::TypeError));
}

// ---- digest (BoringSSL EVP, one-shot) ----

bool digest(const EVP_MD *md, const std::uint8_t *data, std::size_t len, std::uint8_t *out) noexcept {
    unsigned int out_len = 0;
    return EVP_Digest(data, len, out, &out_len, md, nullptr) == 1;
}

// ---- hash.crc32 ----

ScriptResult crc32_fn(void * /*userdata*/, const Library::HostCallFrame & /*frame*/, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        // No value: Java's asText(null) on missing -> empty -> 0.
        return ScriptResult::success(JsValue::make_integer(0));
    }
    std::string text;
    node_as_text(args.args[0], text);
    if (text.empty()) {
        return ScriptResult::success(JsValue::make_integer(0));
    }
    return ScriptResult::success(
            JsValue::make_integer(static_cast<std::int64_t>(crc32_bytes(text.data(), text.size()))));
}

// ---- hash.md5 / sha1 / sha256 ----

// Shared body: String (UTF-8) or Binary in -> lowercase hex out.
ScriptResult digest_fn(const Library::HostCallFrame &frame, const JsValue &arg, const EVP_MD *md,
                       std::size_t digest_len) noexcept {
    std::string_view text;
    const std::uint8_t *data = nullptr;
    std::size_t len = 0;

    if (string_utf8_view(arg, text)) {
        data = reinterpret_cast<const std::uint8_t *>(text.data());
        len = text.size();
    } else if (binary_bytes(arg, data, len)) {
        // data/len set by binary_bytes.
    } else {
        // Java: "<type> not support <algo>" -> ScriptExecException.
        return type_error();
    }

    std::uint8_t out[32]; // SHA-256 is the largest at 32 bytes.
    if (digest_len > sizeof(out) || !digest(md, data, len, out)) {
        // Unreachable for valid MD5/SHA-1/SHA-256 inputs.
        return ScriptResult::abort(ScriptAbortReason::HostFault);
    }

    char hex[64]; // 32 digest bytes -> 64 hex chars.
    hex_encode(out, digest_len, hex);
    JsValue result = JsValue::make_string(*frame.runtime, hex, digest_len * 2);
    if (js_value_type(result) != JsNodeType::String) {
        // make_string returns undefined on OOM; the hex string is never empty.
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return ScriptResult::success(result);
}

ScriptResult md5_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    return digest_fn(frame, args.args[0], EVP_md5(), 16);
}

ScriptResult sha1_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    return digest_fn(frame, args.args[0], EVP_sha1(), 20);
}

ScriptResult sha256_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    if (!args.args || args.argc < 1) {
        return type_error();
    }
    return digest_fn(frame, args.args[0], EVP_sha256(), 32);
}

} // namespace

void register_hash_funcs(StdLibrary &lib) {
    lib.register_func("hash.crc32", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &crc32_fn, nullptr, "hash.crc32");
    lib.register_func("hash.md5", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &md5_fn, nullptr, "hash.md5");
    lib.register_func("hash.sha1", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &sha1_fn, nullptr, "hash.sha1");
    lib.register_func("hash.sha256", Library::FunctionSignature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
                      &sha256_fn, nullptr, "hash.sha256");
}

} // namespace fiber::script::std_lib
