#include "RequestFuncs.h"

#include "ScriptExchangeCtx.h"

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/json/JsonDecode.h"
#include "../common/mem/IoBufChain.h"
#include "../http/HttpExchange.h"
#include "../script/AsyncTask.h"
#include "../script/JsGc.h"
#include "../script/JsValue.h"
#include "../script/Library.h"
#include "../script/ScriptResult.h"
#include "../script/gc/GcInternal.h"
#include "../script/json/JsValueDecode.h"
#include "../script/std/NodeText.h"
#include "../script/std/StdLibrary.h"

#include <cstring>
#include <string>
#include <string_view>

namespace fiber::http_script {

namespace {

using fiber::script::AsyncTask;
using fiber::script::ConstValueHandle;
using fiber::script::GcHeap;
using fiber::script::js_value_type;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ScriptResult;
using fiber::script::ValueHandle;
using fiber::script::std_lib::string_utf8_view;

ScriptExchangeCtx *ctx_of(const Library::HostCallFrame &frame) noexcept {
    return static_cast<ScriptExchangeCtx *>(frame.attach);
}

// Builds a named+message exception JsValue (mirrors Java ScriptExecException(name, msg)).
// Falls back to an OutOfMemory abort if the exception itself cannot be allocated.
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

// Reads the entire request body into out, looping read_body until the chain is complete.
// Mirrors the read loop in example/http1_echo.cpp. Returns the IoErr on failure.
fiber::async::Task<fiber::common::IoResult<void>> read_full_body(fiber::http::HttpExchange &exchange,
                                                                 std::string &out) noexcept {
    for (;;) {
        auto read_result = co_await exchange.read_body(65536);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        fiber::mem::IoBufChain &chain = *read_result;
        while (auto *chunk = chain.front()) {
            auto readable = chunk->readable();
            if (readable == 0) {
                chain.drop_empty_front();
                break;
            }
            out.append(reinterpret_cast<const char *>(chunk->readable_data()), readable);
            chain.consume_and_compact(readable);
        }
        if (chain.complete()) {
            break;
        }
    }
    co_return fiber::common::IoResult<void>{};
}

// ---- req.getHeader() / req.getHeader(name) ----

ScriptResult get_header_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                               Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return ScriptResult::success(ctx->headers());
}

ScriptResult get_header_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                               Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return ScriptResult::success(JsValue::make_null()); // Java: empty/non-text name -> NullNode
    }
    if (!ctx->exchange().request_headers().contains(name_sv)) {
        return ScriptResult::success(JsValue::make_undefined()); // Java MissingNode
    }
    std::string_view value = ctx->exchange().header(name_sv);
    JsValue result = JsValue::make_string(*frame.runtime, value.data(), value.size());
    if (js_value_type(result) != JsNodeType::String) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return ScriptResult::success(result);
}

// ---- req.getQuery() / req.getQuery(name) ----

ScriptResult get_query_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                              Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return ScriptResult::success(ctx->query());
}

ScriptResult lookup_key(GcHeap &heap, JsValue object, std::string_view key) noexcept {
    if (js_value_type(object) != JsNodeType::Object) {
        return ScriptResult::success(JsValue::make_undefined());
    }
    GcHeap::LocalMark mark(heap);
    ValueHandle found = heap.local_value();
    if (!found) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    *found = JsValue::make_undefined();
    if (!fiber::script::gc_object_get_key(&heap, ConstValueHandle(&object), key.data(), key.size(), found)) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (js_value_type(*found) == JsNodeType::Undefined) {
        return ScriptResult::success(JsValue::make_undefined()); // absent
    }
    return ScriptResult::success(*found);
}

ScriptResult get_query_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                              Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return ScriptResult::success(JsValue::make_undefined()); // Java: empty name -> null
    }
    return lookup_key(*frame.runtime, ctx->query(), name_sv);
}

// ---- req.readJson() [async] ----

AsyncTask read_json_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = frame.runtime;
    if (ctx == nullptr || heap == nullptr) {
        co_return invalid_state();
    }
    std::string body;
    auto read_result = co_await read_full_body(ctx->exchange(), body);
    if (!read_result) {
        co_return error_exn(*heap, "read request body failed");
    }
    if (body.empty()) {
        co_return error_exn(*heap, "client did not sent body");
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle out = heap->local_value();
    if (!out) {
        co_return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    fiber::json::ParseError parse_error;
    auto status = fiber::script::json::decode_js_value(*heap, body.data(), body.size(), out, &parse_error);
    if (status != fiber::json::DecodeStatus::Ok && status != fiber::json::DecodeStatus::Complete) {
        co_return error_exn(*heap, "invalid json body");
    }
    co_return ScriptResult::success(*out);
}

// ---- req.readBinary() [async] ----

AsyncTask read_binary_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                         Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = frame.runtime;
    if (ctx == nullptr || heap == nullptr) {
        co_return invalid_state();
    }
    std::string body;
    auto read_result = co_await read_full_body(ctx->exchange(), body);
    if (!read_result) {
        co_return error_exn(*heap, "read request body failed");
    }
    if (body.empty()) {
        // Java: empty/absent body -> BinaryNode of EMPTY_BYTE_ARR (not an error).
        JsValue empty = JsValue::make_binary(*heap, nullptr, 0);
        co_return ScriptResult::success(empty);
    }
    JsValue bin = JsValue::make_binary(*heap, reinterpret_cast<const std::uint8_t *>(body.data()), body.size());
    if (js_value_type(bin) != JsNodeType::Binary) {
        co_return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    co_return ScriptResult::success(bin);
}

// ---- req.discardBody() [async] ----

AsyncTask discard_body_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                          Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        co_return invalid_state();
    }
    auto result = co_await ctx->exchange().discard_body();
    (void) result; // Java fire-and-forget ignores drain errors
    co_return ScriptResult::success(JsValue::make_null());
}

// ---- req.getUri / getPath / getQueryStr / getMethod ----

ScriptResult make_text(const Library::HostCallFrame &frame, std::string_view text) noexcept {
    JsValue result = JsValue::make_string(*frame.runtime, text.data(), text.size());
    if (js_value_type(result) != JsNodeType::String) {
        return ScriptResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return ScriptResult::success(result);
}

ScriptResult get_uri_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                        Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    // unparsed_uri is the raw request-target (path[?query]) -- the closest C++ analogue
    // to Java HttpExchange.getUri() (HttpUri does not carry scheme/host).
    return make_text(frame, ctx->exchange().uri().unparsed_uri);
}

ScriptResult get_path_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                         Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return make_text(frame, ctx->exchange().uri().path);
}

ScriptResult get_query_str_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                              Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return make_text(frame, ctx->exchange().uri().query);
}

ScriptResult get_method_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                           Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return make_text(frame, ctx->exchange().method_view());
}

// ---- req.getCookie() / req.getCookie(name) ----

ScriptResult get_cookie_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                               Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return ScriptResult::success(ctx->cookies());
}

ScriptResult get_cookie_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                               Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return ScriptResult::success(JsValue::make_undefined());
    }
    return lookup_key(*frame.runtime, ctx->cookies(), name_sv);
}

} // namespace

void register_request_funcs(fiber::script::std_lib::StdLibrary &lib) {
    using Sig = Library::FunctionSignature;
    lib.register_func("req.getHeader", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_header_all_fn,
                      nullptr, "req.getHeader");
    lib.register_func("req.getHeader", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &get_header_one_fn,
                      nullptr, "req.getHeader");
    lib.register_func("req.getQuery", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_query_all_fn,
                      nullptr, "req.getQuery");
    lib.register_func("req.getQuery", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &get_query_one_fn,
                      nullptr, "req.getQuery");
    lib.register_async_func("req.readJson", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &read_json_fn,
                            nullptr, "req.readJson");
    lib.register_async_func("req.readBinary", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                            &read_binary_fn, nullptr, "req.readBinary");
    lib.register_async_func("req.discardBody", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false},
                            &discard_body_fn, nullptr, "req.discardBody");
    lib.register_func("req.getUri", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_uri_fn, nullptr,
                      "req.getUri");
    lib.register_func("req.getPath", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_path_fn, nullptr,
                      "req.getPath");
    lib.register_func("req.getQueryStr", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_query_str_fn,
                      nullptr, "req.getQueryStr");
    lib.register_func("req.getMethod", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_method_fn,
                      nullptr, "req.getMethod");
    lib.register_func("req.getCookie", Sig{.required_argc = 0, .fixed_argc = 0, .variadic = false}, &get_cookie_all_fn,
                      nullptr, "req.getCookie");
    lib.register_func("req.getCookie", Sig{.required_argc = 1, .fixed_argc = 1, .variadic = false}, &get_cookie_one_fn,
                      nullptr, "req.getCookie");
}

} // namespace fiber::http_script
