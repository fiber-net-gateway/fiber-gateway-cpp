#include "ResponseFuncs.h"

#include "ScriptExchangeCtx.h"

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/util/CookieCodec.h"
#include "../http/HttpExchange.h"
#include "../script/AsyncTask.h"
#include "../script/JsGc.h"
#include "../script/JsValue.h"
#include "../script/Library.h"
#include "../script/ScriptResult.h"
#include "../script/gc/GcInternal.h"
#include "../script/std/NodeText.h"
#include "../script/std/StdLibrary.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace fiber::http_script {

namespace {

using fiber::script::AsyncTask;
using fiber::script::ConstValueHandle;
using fiber::script::GcHeap;
using fiber::script::js_value_bool;
using fiber::script::js_value_int64;
using fiber::script::js_value_type;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ScriptResult;
using fiber::script::ValueHandle;
using fiber::script::std_lib::binary_bytes;
using fiber::script::std_lib::node_as_text;
using fiber::script::std_lib::string_utf8_view;

ScriptExchangeCtx *ctx_of(const Library::HostCallFrame &frame) noexcept {
    return static_cast<ScriptExchangeCtx *>(frame.attach);
}

ScriptResult error_exn(GcHeap &heap, std::string_view message) noexcept {
    static constexpr char kErrorName[] = "Error";
    GcHeap::LocalMark mark(heap);
    ValueHandle ex = heap.local_value();
    if (!ex) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!fiber::script::gc_make_exception(&heap, ex, -1, kErrorName, sizeof(kErrorName) - 1, message.data(),
                                          message.size())) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return ScriptResult::exception(*ex);
}

ScriptResult invalid_state() noexcept { return ScriptResult::abort(ScriptAbortReason::InvalidState); }

// Reads a named field off an Object as a JsValue (Undefined when absent or non-object).
// gc_object_get_key is a pure hash read (no allocation, no GC), so the returned heap ref
// stays valid for the duration of the sync caller without rooting.
JsValue get_field(GcHeap &heap, JsValue object, const char *key, std::size_t key_len) noexcept {
    if (js_value_type(object) != JsNodeType::Object) {
        return JsValue::make_undefined();
    }
    JsValue out = JsValue::make_undefined();
    fiber::script::gc_object_get_key(&heap, ConstValueHandle(&object), key, key_len, ValueHandle(&out));
    return out;
}

std::string_view field_string(GcHeap &heap, const JsValue &object, const char *key, std::size_t key_len) noexcept {
    JsValue v = get_field(heap, object, key, key_len);
    std::string_view sv;
    return string_utf8_view(v, sv) ? sv : std::string_view{};
}

int as_int(const JsValue &v, int def) noexcept {
    return js_value_type(v) == JsNodeType::Integer ? static_cast<int>(js_value_int64(v)) : def;
}

// ---- resp.setHeader / resp.addHeader (sync) ----

ScriptResult set_header_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 2) {
        return invalid_state();
    }
    std::string_view name;
    if (!string_utf8_view(args.args[0], name)) {
        return error_exn(frame.runtime, "set header require string key value");
    }
    std::string value_buf;
    node_as_text(args.args[1], value_buf);
    if (name.empty() || value_buf.empty()) {
        return error_exn(frame.runtime, "set header require string key value");
    }
    ctx->set_response_header(name, value_buf);
    return ScriptResult::success(JsValue::make_null());
}

ScriptResult add_header_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 2) {
        return invalid_state();
    }
    std::string_view name;
    if (!string_utf8_view(args.args[0], name)) {
        return error_exn(frame.runtime, "add header require string key value");
    }
    std::string value_buf;
    node_as_text(args.args[1], value_buf);
    if (name.empty() || value_buf.empty()) {
        return error_exn(frame.runtime, "add header require string key value");
    }
    ctx->add_response_header(name, value_buf);
    return ScriptResult::success(JsValue::make_null());
}

// ---- resp.sendJson(status, body) [async] ----

AsyncTask send_json_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 2) {
        co_return invalid_state();
    }
    int status = as_int(args.args[0], 200);
    const JsValue &body = args.args[1];
    auto result = co_await ctx->write_json(status, body);
    if (!result) {
        co_return error_exn(*heap, "error send json");
    }
    co_return ScriptResult::success(JsValue::make_null());
}

// ---- resp.send(status) [async] ----

AsyncTask send_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 1) {
        co_return invalid_state();
    }
    int status = as_int(args.args[0], 200);
    auto result = co_await ctx->write_empty(status);
    if (!result) {
        co_return error_exn(*heap, "error send");
    }
    co_return ScriptResult::success(JsValue::make_null());
}

// ---- resp.send(status, body) [async] ----

AsyncTask send_two_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 2) {
        co_return invalid_state();
    }
    int status = as_int(args.args[0], 200);
    const JsValue &body = args.args[1];
    JsNodeType kind = js_value_type(body);

    if (kind == JsNodeType::Binary) {
        const std::uint8_t *data = nullptr;
        std::size_t len = 0;
        if (!binary_bytes(body, data, len)) {
            co_return error_exn(*heap, "error write binary response");
        }
        auto result = co_await ctx->write_raw_bytes(status, data, len);
        if (!result) {
            co_return error_exn(*heap, "error write binary response");
        }
        co_return ScriptResult::success(JsValue::make_null());
    }

    if (kind == JsNodeType::String) {
        std::string text;
        std::string_view sv;
        if (string_utf8_view(body, sv)) {
            // zero-copy into the rooted heap string
        } else {
            node_as_text(body, text); // best-effort for strings without a strict UTF-8 view
            sv = text;
        }
        ctx->set_response_header("Content-Type", "text/plain;charset=utf-8");
        auto result =
                co_await ctx->write_raw_bytes(status, reinterpret_cast<const std::uint8_t *>(sv.data()), sv.size());
        if (!result) {
            co_return error_exn(*heap, "error textual response");
        }
        co_return ScriptResult::success(JsValue::make_null());
    }

    // Numbers / objects / arrays / null / boolean -> JSON.
    auto result = co_await ctx->write_json(status, body);
    if (!result) {
        co_return error_exn(*heap, "error send json");
    }
    co_return ScriptResult::success(JsValue::make_null());
}

// ---- resp.addCookie(cookie) (sync) ----

ScriptResult add_cookie_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &node = args.args[0];
    if (js_value_type(node) != JsNodeType::Object) {
        return ScriptResult::success(JsValue::make_boolean(false));
    }

    std::string_view name = field_string(*heap, node, "name", 4);
    if (name.empty()) {
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    std::string value_buf;
    node_as_text(get_field(*heap, node, "value", 5), value_buf);
    std::string_view domain = field_string(*heap, node, "domain", 6);
    std::string_view path = field_string(*heap, node, "path", 4);
    JsValue max_age_v = get_field(*heap, node, "maxAge", 7);
    std::int64_t max_age = js_value_type(max_age_v) == JsNodeType::Integer ? js_value_int64(max_age_v) : -1;
    JsValue secure_v = get_field(*heap, node, "secure", 6);
    bool secure = js_value_type(secure_v) == JsNodeType::Boolean && js_value_bool(secure_v);
    JsValue http_only_v = get_field(*heap, node, "httpOnly", 8);
    bool http_only = js_value_type(http_only_v) == JsNodeType::Boolean && js_value_bool(http_only_v);
    std::string_view same_site = field_string(*heap, node, "sameSite", 8);

    fiber::util::Cookie cookie{};
    cookie.name = name;
    cookie.value = value_buf;
    cookie.domain = domain;
    cookie.path = path;
    cookie.max_age = max_age;
    cookie.secure = secure;
    cookie.http_only = http_only;
    if (same_site == "Lax") {
        cookie.same_site = fiber::util::CookieSameSite::Lax;
    } else if (same_site == "Strict") {
        cookie.same_site = fiber::util::CookieSameSite::Strict;
    } else if (same_site == "None") {
        cookie.same_site = fiber::util::CookieSameSite::None;
    } else {
        cookie.same_site = fiber::util::CookieSameSite::Unset;
    }

    std::string encoded;
    if (!fiber::util::encode_set_cookie(cookie, encoded)) {
        return ScriptResult::success(JsValue::make_boolean(false));
    }
    ctx->add_response_header("Set-Cookie", encoded);
    return ScriptResult::success(JsValue::make_boolean(true));
}

} // namespace

void register_response_funcs(fiber::script::std_lib::StdLibrary &lib) {
    using Sig = Library::FunctionSignature;
    lib.register_func("resp.setHeader", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &set_header_fn,
                      nullptr, "resp.setHeader");
    lib.register_func("resp.addHeader", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &add_header_fn,
                      nullptr, "resp.addHeader");
    lib.register_async_func("resp.sendJson", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &send_json_fn,
                            nullptr, "resp.sendJson");
    lib.register_async_func("resp.send", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &send_one_fn,
                            nullptr, "resp.send");
    lib.register_async_func("resp.send", Sig{.required_argc = 2, .fixed_argc = 2, .variadic = false}, &send_two_fn,
                            nullptr, "resp.send");
    lib.register_func("resp.addCookie", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &add_cookie_fn,
                      nullptr, "resp.addCookie");
}

} // namespace fiber::http_script
