#include <fiber/http_script/RequestFuncs.h>

#include <fiber/http_script/ScriptExchangeCtx.h>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/json/JsonDecode.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/http/HttpBodyPipe.h>
#include <fiber/http/HttpBodySpec.h>
#include <fiber/http/HttpExchange.h>
#include <fiber/script/AsyncTask.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/Library.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/gc/GcInternal.h>
#include <fiber/script/json/JsValueDecode.h>
#include <fiber/script/std/NodeText.h>
#include <fiber/script/std/StdLibrary.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

namespace fiber::http_script {

namespace {

using fiber::script::AbiResult;
using fiber::script::AsyncTask;
using fiber::script::ConstValueHandle;
using fiber::script::GcBinary;
using fiber::script::GcHeap;
using fiber::script::GcHeapKind;
using fiber::script::js_make_heap_ref;
using fiber::script::js_value_type;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ValueHandle;
using fiber::script::json::DecodeStatus;
using fiber::script::std_lib::string_utf8_view;
using JsonValueDecoder = fiber::script::json::JsValueStreamDecoder;

constexpr std::size_t kBodyReadBytes = 65536;
// Tighter than the pipe's default: a pipe holds one buffer, a body held for a GC copy can
// reach the heap limit, so what it pins must stay close to what it counts.
constexpr std::size_t kHeldBodyCoalesceRatio = 2;
constexpr std::string_view kBodyConsumedMessage = "request body already consumed";
// Enough for an exception's name and message, with room to spare.
constexpr std::size_t kExceptionBytes = 1024;

ScriptExchangeCtx *ctx_of(const Library::HostCallFrame &frame) noexcept {
    return static_cast<ScriptExchangeCtx *>(frame.attach);
}

// Builds a named+message exception JsValue (mirrors Java ScriptExecException(name, msg)).
// Falls back to an OutOfMemory abort if the exception itself cannot be allocated.
AbiResult named_exn(GcHeap &heap, std::string_view name, std::string_view message) noexcept {
    // After a refusal the heap may be full of what the failed attempt built, and an exception
    // that cannot be allocated turns a catchable failure into an abort.
    (void) heap.ensure_can_alloc(kExceptionBytes);
    GcHeap::LocalMark mark(heap);
    ValueHandle ex = heap.local_value();
    if (!ex) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!fiber::script::gc_make_exception(&heap, ex, -1, name.data(), name.size(), message.data(), message.size())) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::exception(*ex);
}

AbiResult error_exn(GcHeap &heap, std::string_view message) noexcept { return named_exn(heap, "Error", message); }

// Catchable, unlike the abort of a real allocation failure: the heap is healthy, the body
// just does not fit, so a script may answer with its own response.
AbiResult body_too_large(GcHeap &heap) noexcept {
    return named_exn(heap, "OutOfMemory", "request body exceeds the script heap limit");
}

AbiResult body_read_failed(GcHeap &heap, fiber::common::IoErr error) noexcept {
    if (error == fiber::common::IoErr::NoMem) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return error_exn(heap, "read request body failed");
}

// Claims the one-shot request body for the caller; false when it is already taken.
bool claim_request_body(ScriptExchangeCtx &ctx) noexcept {
    if (ctx.request_body_consumed()) {
        return false;
    }
    ctx.mark_request_body_consumed();
    return true;
}

// What a binary of len bytes takes from the heap, for checking before allocating it.
std::size_t binary_footprint(std::size_t len) noexcept {
    return len == 0 ? 0 : fiber::script::gc_detail::saturating_add(len, sizeof(GcBinary));
}

// Allocates a binary of len uninitialized bytes into out. Nothing stays unrooted here, so a
// collection the allocation asked for would reclaim nothing and is dropped.
GcBinary *new_body_binary(GcHeap &heap, ValueHandle out, std::size_t len) noexcept {
    GcHeap::NoGcScope no_gc(heap, /*defer_collect=*/true);
    GcBinary *bin = fiber::script::gc_new_binary_uninit(&heap, len);
    if (bin != nullptr) {
        *out = js_make_heap_ref(&bin->hdr, GcHeapKind::Binary);
    }
    return bin;
}

AbiResult invalid_state() noexcept { return AbiResult::abort(ScriptAbortReason::InvalidState); }

enum class JsonBodyOutcome : std::uint8_t { Decoded, Empty, Invalid, TooLarge, ReadFailed };

struct JsonBodyResult {
    JsonBodyOutcome outcome = JsonBodyOutcome::Decoded;
    fiber::common::IoErr read_error = fiber::common::IoErr::None;
    bool body_complete = false;
};

[[nodiscard]] JsonBodyResult decode_failure(const JsonValueDecoder &decoder, bool body_complete) noexcept {
    return {.outcome = decoder.error().out_of_memory ? JsonBodyOutcome::TooLarge : JsonBodyOutcome::Invalid,
            .body_complete = body_complete};
}

// Decodes the body into out as it arrives, so it never exists as one piece of text. Its own
// LocalMark ends with it: after a failure, what was built is garbage the caller can reclaim.
fiber::async::Task<JsonBodyResult> decode_json_body(ScriptRequestBody request_body, GcHeap &heap,
                                                    ValueHandle out) noexcept {
    GcHeap::LocalMark mark(heap);
    JsonValueDecoder decoder(heap, out);
    bool saw_bytes = false;
    for (;;) {
        auto read_result = co_await request_body.read(kBodyReadBytes, kBodyIdleTimeout);
        if (!read_result) {
            co_return JsonBodyResult{.outcome = JsonBodyOutcome::ReadFailed, .read_error = read_result.error()};
        }
        fiber::mem::IoBufChain &chain = *read_result;
        const bool complete = chain.complete();
        while (fiber::mem::IoBuf *node = chain.front()) {
            const std::size_t bytes = node->readable();
            if (bytes == 0) {
                chain.drop_empty_front();
                continue;
            }
            saw_bytes = true;
            const DecodeStatus status = decoder.feed(reinterpret_cast<const char *>(node->readable_data()), bytes);
            chain.consume_and_compact(bytes);
            if (status == DecodeStatus::Error || status == DecodeStatus::Canceled) {
                co_return decode_failure(decoder, complete);
            }
        }
        // A token spanning reads is outside the heap until it completes, and has to fit in it
        // then. Checked here because nothing else stops a client sending one without end.
        if (!heap.ensure_can_alloc(decoder.pending_bytes())) {
            *out = JsValue::make_undefined();
            co_return JsonBodyResult{.outcome = JsonBodyOutcome::TooLarge, .body_complete = complete};
        }
        if (complete) {
            break;
        }
    }
    if (!saw_bytes) {
        co_return JsonBodyResult{.outcome = JsonBodyOutcome::Empty, .body_complete = true};
    }
    const DecodeStatus status = decoder.finish();
    if (status == DecodeStatus::Error || status == DecodeStatus::Canceled) {
        co_return decode_failure(decoder, true);
    }
    co_return JsonBodyResult{.outcome = JsonBodyOutcome::Decoded, .body_complete = true};
}

// ---- req.getHeader() / req.getHeader(name) ----

AbiResult get_header_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return AbiResult::success(ctx->headers());
}

AbiResult get_header_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return AbiResult::success(JsValue::make_null()); // Java: empty/non-text name -> NullNode
    }
    if (!ctx->exchange().request_headers().contains(name_sv)) {
        return AbiResult::success(JsValue::make_undefined()); // Java MissingNode
    }
    std::string_view value = ctx->exchange().header(name_sv);
    JsValue result = JsValue::make_string(frame.runtime, value.data(), value.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

// ---- req.getQuery() / req.getQuery(name) ----

AbiResult get_query_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                           Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return AbiResult::success(ctx->query());
}

AbiResult lookup_key(GcHeap &heap, JsValue object, std::string_view key) noexcept {
    return ScriptExchangeCtx::lookup_property(heap, object, key);
}

AbiResult get_query_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return AbiResult::success(JsValue::make_undefined()); // Java: empty name -> null
    }
    return lookup_key(frame.runtime, ctx->query(), name_sv);
}

// ---- req.readJson() [async] ----

AsyncTask read_json_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr) {
        co_return invalid_state();
    }
    if (!claim_request_body(*ctx)) {
        co_return error_exn(*heap, kBodyConsumedMessage);
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle out = heap->local_value();
    if (!out) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    const ScriptRequestBody request_body = ctx->request_body();
    const JsonBodyResult result = co_await decode_json_body(request_body, *heap, out);
    switch (result.outcome) {
        case JsonBodyOutcome::Decoded:
            co_return AbiResult::success(*out);
        case JsonBodyOutcome::Empty:
            co_return error_exn(*heap, "client did not sent body");
        case JsonBodyOutcome::ReadFailed:
            co_return body_read_failed(*heap, result.read_error);
        case JsonBodyOutcome::Invalid:
        case JsonBodyOutcome::TooLarge:
            break;
    }
    // The script may catch this and carry on, so leave no half-read body behind.
    if (!result.body_complete) {
        (void) co_await request_body.discard(kBodyIdleTimeout);
    }
    co_return result.outcome == JsonBodyOutcome::TooLarge ? body_too_large(*heap)
                                                          : error_exn(*heap, "invalid json body");
}

// ---- req.readBinary() [async] ----

AsyncTask read_binary_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                         Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr) {
        co_return invalid_state();
    }
    if (!claim_request_body(*ctx)) {
        co_return error_exn(*heap, kBodyConsumedMessage);
    }
    const ScriptRequestBody request_body = ctx->request_body();
    fiber::http::HttpExchange &exchange = ctx->exchange();

    const fiber::http::HttpBodySpec spec = exchange.request_body_spec();
    const std::size_t declared = spec.is_content_length() ? spec.content_length() : 0;
    if (!heap->ensure_can_alloc(binary_footprint(declared))) {
        // Nothing was read, and a client waiting on 100-continue has sent nothing: reading
        // would answer it with an invitation to upload the body being refused.
        if (exchange.expect_header() == nullptr) {
            (void) co_await request_body.discard(kBodyIdleTimeout);
        }
        co_return body_too_large(*heap);
    }

    // A declared length lets each read go straight to the binary: one copy, and nothing held
    // between reads. The heap cannot change while the script waits here, so the check above
    // stays true.
    if (declared != 0) {
        GcHeap::LocalMark mark(*heap);
        ValueHandle out = heap->local_value();
        if (!out) {
            co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
        GcBinary *bin = new_body_binary(*heap, out, declared);
        if (bin == nullptr) {
            co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
        auto read_result = co_await read_body_into(request_body, bin->data, declared, kBodyIdleTimeout);
        if (!read_result) {
            co_return body_read_failed(*heap, read_result.error());
        }
        co_return AbiResult::success(*out);
    }

    // Length unknown: hold the reads, stopping as soon as the body cannot fit.
    fiber::mem::IoBufChain body;
    for (;;) {
        auto read_result = co_await request_body.read(kBodyReadBytes, kBodyIdleTimeout);
        if (!read_result) {
            co_return body_read_failed(*heap, read_result.error());
        }
        const bool complete = read_result->complete();
        const std::size_t total = body.readable_bytes() + read_result->readable_bytes();
        if (!heap->ensure_can_alloc(binary_footprint(total))) {
            body.clear();
            read_result->clear();
            // The script may catch this and carry on, so leave no half-read body behind.
            (void) co_await request_body.discard(kBodyIdleTimeout);
            co_return body_too_large(*heap);
        }
        if (!fiber::http::append_coalescing(body, std::move(*read_result), fiber::http::kBodyCoalesceCapacity,
                                            kHeldBodyCoalesceRatio)) {
            co_return body_read_failed(*heap, fiber::common::IoErr::Invalid);
        }
        if (complete) {
            break;
        }
    }

    const std::size_t len = body.readable_bytes();
    if (len == 0) {
        // Java: empty/absent body -> BinaryNode of EMPTY_BYTE_ARR (not an error).
        co_return AbiResult::success(JsValue::make_binary(*heap, nullptr, 0));
    }
    GcHeap::LocalMark mark(*heap);
    ValueHandle out = heap->local_value();
    if (!out) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    GcBinary *bin = new_body_binary(*heap, out, len);
    if (bin == nullptr) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    drain_chain_into(body, bin->data);
    co_return AbiResult::success(*out);
}

// ---- req.discardBody() [async] ----

AsyncTask discard_body_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                          Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        co_return invalid_state();
    }
    ctx->mark_request_body_consumed();
    auto result = co_await ctx->request_body().discard(kBodyIdleTimeout);
    (void) result; // Java fire-and-forget ignores drain errors
    co_return AbiResult::success(JsValue::make_null());
}

// ---- req.getUri / getPath / getQueryStr / getMethod ----

AbiResult make_text(const Library::HostCallFrame &frame, std::string_view text) noexcept {
    JsValue result = JsValue::make_string(frame.runtime, text.data(), text.size());
    if (js_value_type(result) != JsNodeType::String) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::success(result);
}

AbiResult get_uri_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    // unparsed_uri is the raw request-target (path[?query]) -- the closest C++ analogue
    // to Java HttpExchange.getUri() (HttpUri does not carry scheme/host).
    return make_text(frame, ctx->exchange().uri().unparsed_uri);
}

AbiResult get_path_fn(void * /*userdata*/, const Library::HostCallFrame &frame, Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return make_text(frame, ctx->exchange().uri().path);
}

AbiResult get_query_str_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                           Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return make_text(frame, ctx->exchange().uri().query);
}

AbiResult get_method_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                        Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return make_text(frame, ctx->exchange().method_view());
}

// ---- req.getCookie() / req.getCookie(name) ----

AbiResult get_cookie_all_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments /*args*/) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr) {
        return invalid_state();
    }
    return AbiResult::success(ctx->cookies());
}

AbiResult get_cookie_one_fn(void * /*userdata*/, const Library::HostCallFrame &frame,
                            Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    if (ctx == nullptr || args.args == nullptr || args.argc < 1) {
        return invalid_state();
    }
    const JsValue &name = args.args[0];
    std::string_view name_sv;
    if (!string_utf8_view(name, name_sv) || name_sv.empty()) {
        return AbiResult::success(JsValue::make_undefined());
    }
    return lookup_key(frame.runtime, ctx->cookies(), name_sv);
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
