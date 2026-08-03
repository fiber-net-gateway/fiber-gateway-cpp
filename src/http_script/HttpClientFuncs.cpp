#include "HttpClientFuncs.h"

#include "ScriptExchangeCtx.h"

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/json/JsonEncode.h"
#include "../common/util/UrlForm.h"
#include "../event/EventLoop.h"
#include "../http/ClientHttp1Exchange.h"
#include "../http/ClientHttp1Types.h"
#include "../http/Http1ClientConnection.h"
#include "../http/HttpBodyPipe.h"
#include "../http/HttpBodySpec.h"
#include "../http/HttpCommon.h"
#include "../http/HttpExchange.h"
#include "../http/HttpExchangeIo.h"
#include "../http/HttpHeaderHash.h"
#include "../http/HttpHeaders.h"
#include "../http/HttpProxyCore.h"
#include "../http/HttpWebSocketProxy.h"
#include "../script/AsyncTask.h"
#include "../script/JsGc.h"
#include "../script/JsValue.h"
#include "../script/Library.h"
#include "../script/ScriptResult.h"
#include "../script/gc/GcInternal.h"
#include "../script/json/JsValueEncode.h"
#include "../script/std/NodeText.h"
#include "../script/std/StdLibrary.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace fiber::http_script {

using fiber::script::AbiResult;
using fiber::script::AsyncTask;
using fiber::script::ConstValueHandle;
using fiber::script::GcHeap;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;
using fiber::script::ScriptAbortReason;
using fiber::script::ValueHandle;
using fiber::script::std_lib::binary_bytes;
using fiber::script::std_lib::node_as_text;
using fiber::script::std_lib::node_json_to_string;
using fiber::script::std_lib::string_utf8_view;
using namespace fiber::http::proxy_core;

namespace {

constexpr std::chrono::milliseconds kDefaultTimeout{30000};

// Pre-hashed header names for per-response lookups.
constexpr std::uint64_t kContentTypeHash = fiber::http::http_header_name_hash("content-type");
constexpr std::uint64_t kContentLengthHash = fiber::http::http_header_name_hash("content-length");

bool icase_starts_with(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

ScriptExchangeCtx *ctx_of(const Library::HostCallFrame &frame) noexcept {
    return static_cast<ScriptExchangeCtx *>(frame.attach);
}

AbiResult error_exn(GcHeap &heap, std::string_view message) noexcept {
    static constexpr char kErrorName[] = "Error";
    GcHeap::LocalMark mark(heap);
    ValueHandle ex = heap.local_value();
    if (!ex) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    if (!fiber::script::gc_make_exception(&heap, ex, -1, kErrorName, sizeof(kErrorName) - 1, message.data(),
                                          message.size())) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    return AbiResult::exception(*ex);
}

AsyncTask invalid_state_async() noexcept { co_return AbiResult::abort(ScriptAbortReason::InvalidState); }

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

int field_int(GcHeap &heap, const JsValue &object, const char *key, std::size_t key_len, int def) noexcept {
    JsValue v = get_field(heap, object, key, key_len);
    return js_value_type(v) == JsNodeType::Integer ? static_cast<int>(fiber::script::js_value_int64(v)) : def;
}

bool field_bool(GcHeap &heap, const JsValue &object, const char *key, std::size_t key_len, bool def) noexcept {
    JsValue v = get_field(heap, object, key, key_len);
    return js_value_type(v) == JsNodeType::Boolean ? fiber::script::js_value_bool(v) : def;
}

std::chrono::milliseconds resolve_timeout(GcHeap &heap, const JsValue &options) noexcept {
    JsValue v = get_field(heap, options, "timeout", 7);
    if (js_value_type(v) == JsNodeType::Integer) {
        const auto ms = fiber::script::js_value_int64(v);
        if (ms > 0) {
            return std::chrono::milliseconds(ms);
        }
    }
    return kDefaultTimeout;
}

fiber::http::HttpMethod resolve_http_method(std::string_view name, fiber::http::HttpMethod def) noexcept {
    auto eq = [](std::string_view a, const char *b) noexcept {
        const std::size_t n = std::strlen(b);
        if (a.size() != n) {
            return false;
        }
        for (std::size_t i = 0; i < n; ++i) {
            char c = a[i];
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
            if (c != b[i]) {
                return false;
            }
        }
        return true;
    };
    if (eq(name, "GET"))
        return fiber::http::HttpMethod::Get;
    if (eq(name, "POST"))
        return fiber::http::HttpMethod::Post;
    if (eq(name, "PUT"))
        return fiber::http::HttpMethod::Put;
    if (eq(name, "DELETE"))
        return fiber::http::HttpMethod::Delete;
    if (eq(name, "HEAD"))
        return fiber::http::HttpMethod::Head;
    if (eq(name, "OPTIONS"))
        return fiber::http::HttpMethod::Options;
    if (eq(name, "PATCH"))
        return fiber::http::HttpMethod::Patch;
    return def;
}

// Appends "k=v&..." for an Object's fields (arrays expand to repeated keys), mirroring Java's
// UrlEncoded.encodeInto query/form construction.
void object_pairs_to_form(GcHeap &heap, const JsValue &obj, std::string &out) noexcept {
    if (js_value_type(obj) != JsNodeType::Object) {
        return;
    }
    const fiber::script::GcObject *o = fiber::script::js_value_heap_ptr<fiber::script::GcObject>(obj);
    std::string key, value;
    for (const fiber::script::GcObjectEntry *e = fiber::script::gc_object_first_entry(o); e != nullptr;
         e = fiber::script::gc_object_next_entry(o, e)) {
        if (!e->occupied || !e->key) {
            continue;
        }
        fiber::script::gc_string_to_utf8(e->key, key);
        const JsValue &v = e->value;
        if (js_value_type(v) == JsNodeType::Array) {
            const fiber::script::GcArray *a = fiber::script::js_value_heap_ptr<fiber::script::GcArray>(v);
            for (std::size_t i = 0; i < a->size; ++i) {
                value.clear();
                fiber::script::std_lib::node_json_to_string(a->elems[i], value);
                fiber::util::form_encode(key, out);
                out.push_back('=');
                fiber::util::form_encode(value, out);
                out.push_back('&');
            }
        } else {
            value.clear();
            fiber::script::std_lib::node_json_to_string(v, value);
            fiber::util::form_encode(key, out);
            out.push_back('=');
            fiber::util::form_encode(value, out);
            out.push_back('&');
        }
    }
    if (!out.empty() && out.back() == '&') {
        out.pop_back();
    }
}

// Builds the request-target (path[?query]) for http.request. `url` (if present) is the full
// path?query and takes precedence; otherwise `path` + `query` (string or Object) are used.
std::string build_request_target(GcHeap &heap, const JsValue &options, std::string_view default_path) {
    const std::string_view url_sv = field_string(heap, options, "url", 3);
    if (!url_sv.empty()) {
        return std::string(url_sv);
    }
    std::string path(default_path);
    const std::string_view pv = field_string(heap, options, "path", 4);
    if (!pv.empty()) {
        path.assign(pv);
    }
    JsValue qv = get_field(heap, options, "query", 5);
    std::string_view qsv;
    if (string_utf8_view(qv, qsv) && !qsv.empty()) {
        path.push_back('?');
        path.append(qsv);
    } else if (js_value_type(qv) == JsNodeType::Object) {
        std::string qs;
        object_pairs_to_form(heap, qv, qs);
        if (!qs.empty()) {
            path.push_back('?');
            path.append(qs);
        }
    }
    return path;
}

// Applies options.headers (Object: name -> string|null; null removes) onto the request headers.
void apply_options_headers(GcHeap &heap, const JsValue &options, fiber::http::HttpHeaders &headers) noexcept {
    JsValue hv = get_field(heap, options, "headers", 7);
    if (js_value_type(hv) != JsNodeType::Object) {
        return;
    }
    const fiber::script::GcObject *o = fiber::script::js_value_heap_ptr<fiber::script::GcObject>(hv);
    std::string key, value;
    for (const fiber::script::GcObjectEntry *e = fiber::script::gc_object_first_entry(o); e != nullptr;
         e = fiber::script::gc_object_next_entry(o, e)) {
        if (!e->occupied || !e->key) {
            continue;
        }
        fiber::script::gc_string_to_utf8(e->key, key);
        const JsValue &v = e->value;
        if (js_value_type(v) == JsNodeType::Null || js_value_type(v) == JsNodeType::Undefined) {
            headers.remove(key);
            continue;
        }
        value.clear();
        fiber::script::std_lib::node_json_to_string(v, value);
        if (value.empty()) {
            continue;
        }
        headers.set(key, value);
    }
}

// Applies options.responseHeaders (Object: name -> string|null; null removes) onto a
// downstream response header block.
void apply_options_response_headers(GcHeap &heap, const JsValue &options, fiber::http::HttpHeaders &headers) noexcept {
    JsValue rhv = get_field(heap, options, "responseHeaders", 15);
    if (js_value_type(rhv) != JsNodeType::Object) {
        return;
    }
    const fiber::script::GcObject *object = fiber::script::js_value_heap_ptr<fiber::script::GcObject>(rhv);
    std::string key;
    std::string value;
    for (const fiber::script::GcObjectEntry *entry = fiber::script::gc_object_first_entry(object); entry != nullptr;
         entry = fiber::script::gc_object_next_entry(object, entry)) {
        if (!entry->occupied || !entry->key) {
            continue;
        }
        fiber::script::gc_string_to_utf8(entry->key, key);
        const JsValue &item = entry->value;
        if (js_value_type(item) == JsNodeType::Null || js_value_type(item) == JsNodeType::Undefined) {
            headers.remove(key);
            continue;
        }
        value.clear();
        fiber::script::std_lib::node_json_to_string(item, value);
        if (!value.empty()) {
            headers.set(key, value);
        }
    }
}

// Serializes a JsValue body to JSON bytes.
bool json_encode_value(GcHeap &heap, const JsValue &body, std::string &out) noexcept {
    struct StringSink final : public fiber::json::OutputSink {
        explicit StringSink(std::string &o) noexcept : out(o) {}
        bool write(const char *data, std::size_t len) noexcept override {
            out.append(data, len);
            return true;
        }
        std::string &out;
    };
    StringSink sink(out);
    fiber::json::Generator gen(sink);
    return fiber::script::json::encode_js_value(gen, body) == fiber::json::Generator::Result::OK ||
           fiber::script::json::encode_js_value(gen, body) == fiber::json::Generator::Result::GenerateComplete;
}

// Resolves options.body to bytes + sets content-type when unset. Returns the body framing;
// end_stream=true when there is no body (send_header can close the request).
fiber::http::HttpBodySpec build_request_body(GcHeap &heap, const JsValue &options, fiber::http::HttpHeaders &headers,
                                             std::string &body_bytes, bool &end_stream) noexcept {
    JsValue body = get_field(heap, options, "body", 4);
    end_stream = true;
    const JsNodeType bt = js_value_type(body);
    if (bt == JsNodeType::Undefined || bt == JsNodeType::Null) {
        return fiber::http::HttpBodySpec::None();
    }
    const bool has_ct = !headers.get("content-type", kContentTypeHash).empty();

    if (bt == JsNodeType::Binary) {
        const std::uint8_t *d = nullptr;
        std::size_t n = 0;
        if (!binary_bytes(body, d, n) || n == 0) {
            return fiber::http::HttpBodySpec::None();
        }
        if (!has_ct) {
            headers.set("Content-Type", "application/octet-stream");
        }
        body_bytes.assign(reinterpret_cast<const char *>(d), n);
    } else if (bt == JsNodeType::String) {
        std::string_view sv;
        if (string_utf8_view(body, sv)) {
            body_bytes.assign(sv);
        } else {
            node_as_text(body, body_bytes);
        }
        if (body_bytes.empty()) {
            return fiber::http::HttpBodySpec::None();
        }
        if (!has_ct) {
            headers.set("Content-Type", "text/plain;charset=utf-8");
        }
    } else if (bt == JsNodeType::Object) {
        const std::string_view ct = headers.get("content-type", kContentTypeHash);
        if (ct.find("application/x-www-form-urlencoded") != std::string_view::npos) {
            object_pairs_to_form(heap, body, body_bytes);
            if (!has_ct) {
                headers.set("Content-Type", "application/x-www-form-urlencoded");
            }
            if (body_bytes.empty()) {
                return fiber::http::HttpBodySpec::None();
            }
        } else {
            if (!json_encode_value(heap, body, body_bytes)) {
                return fiber::http::HttpBodySpec::None();
            }
            if (!has_ct) {
                headers.set("Content-Type", "application/json;charset=utf-8");
            }
        }
    } else {
        // number / boolean / array -> JSON
        if (!json_encode_value(heap, body, body_bytes)) {
            return fiber::http::HttpBodySpec::None();
        }
        if (!has_ct) {
            headers.set("Content-Type", "application/json;charset=utf-8");
        }
    }

    end_stream = false;
    return fiber::http::HttpBodySpec::ContentLength(body_bytes.size());
}

// The upstream host target is bound once at compile time via `directive <name> = http "<target>";`
// and reaches the call as userdata (the HttpDirectiveDef). There is no runtime target resolution
// from the options object -- the host never comes from options.
std::optional<HttpTargetSpec> resolve_target(void *userdata) noexcept {
    if (userdata == nullptr) {
        return std::nullopt;
    }
    return static_cast<const HttpDirectiveDef *>(userdata)->target();
}

// options.url is the request path?query (e.g. "/items?q=1"), never a host. Reject the common
// mistake of putting a full http(s)://host URL there -- the host must come from the directive
// binding. Returns an error message when url is a disallowed host-form URL, otherwise nullopt.
std::optional<std::string> url_field_error(std::string_view call, GcHeap &heap, const JsValue &options) noexcept {
    const std::string_view sv = field_string(heap, options, "url", 3);
    if (sv.empty()) {
        return std::nullopt;
    }
    if (icase_starts_with(sv, "http://") || icase_starts_with(sv, "https://")) {
        std::string msg;
        msg.reserve(call.size() + 128);
        msg.append(call);
        msg.append(": 'url' is the request path?query (e.g. \"/items?q=1\"), not a host; "
                   "bind the upstream host with: directive svc = http \"<host>\";");
        return msg;
    }
    return std::nullopt;
}

// Acquires a connected upstream connection via the app's services. The returned holder owns the
// pool lease (or transient connection) for the call's lifetime; services.acquire performs the
// pool lookup + (DNS-on-miss) connect internally.
struct AcquiredConnection {
    std::unique_ptr<HttpUpstreamConnection> holder;
    fiber::http::Http1ClientConnection *conn = nullptr;
};

fiber::async::Task<fiber::common::IoResult<AcquiredConnection>>
acquire_and_connect(HttpScriptServices &services, const HttpTargetSpec &target,
                    std::chrono::milliseconds timeout) noexcept {
    AcquiredConnection out;
    auto acquire_result = co_await services.acquire(target, timeout);
    if (!acquire_result) {
        co_return std::unexpected(acquire_result.error());
    }
    out.holder = std::move(*acquire_result);
    out.conn = &out.holder->connection();
    co_return out;
}

// ---- GC object field setters (root the value across the set_key allocation) ----

bool gc_put_value(GcHeap &heap, ValueHandle obj_root, const char *key, std::size_t key_len, JsValue value) noexcept {
    GcHeap::LocalMark mark(heap);
    ValueHandle item = heap.local_value();
    if (!item) {
        return false;
    }
    *item = value;
    return fiber::script::gc_object_set_key(&heap, obj_root, key, key_len, *item);
}

bool gc_put_string(GcHeap &heap, ValueHandle obj_root, const char *key, std::size_t key_len,
                   std::string_view value) noexcept {
    GcHeap::LocalMark mark(heap);
    ValueHandle item = heap.local_value();
    if (!item) {
        return false;
    }
    *item = JsValue::make_string(heap, value.data(), value.size());
    if (js_value_type(*item) != JsNodeType::String) {
        return false;
    }
    return fiber::script::gc_object_set_key(&heap, obj_root, key, key_len, *item);
}

bool gc_put_binary(GcHeap &heap, ValueHandle obj_root, const char *key, std::size_t key_len, const std::uint8_t *data,
                   std::size_t len) noexcept {
    GcHeap::LocalMark mark(heap);
    ValueHandle item = heap.local_value();
    if (!item) {
        return false;
    }
    *item = JsValue::make_binary(heap, data, len);
    if (js_value_type(*item) != JsNodeType::Binary) {
        return false;
    }
    return fiber::script::gc_object_set_key(&heap, obj_root, key, key_len, *item);
}

// Reads the full upstream response body into a contiguous string.
fiber::async::Task<fiber::common::IoResult<void>> read_full_response_body(fiber::http::ClientHttp1Exchange &exchange,
                                                                          std::string &out,
                                                                          std::chrono::milliseconds timeout) noexcept {
    for (;;) {
        auto read_result = co_await exchange.read_body(kBodyChunkSize, timeout);
        if (!read_result) {
            co_return std::unexpected(read_result.error());
        }
        fiber::mem::IoBufChain &chain = *read_result;
        while (auto *chunk = chain.front()) {
            const auto readable = chunk->readable();
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

} // namespace

// ---- http.request(options) -> {status, headers?, body} ----

AsyncTask http_request_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || ctx->services() == nullptr) {
        co_return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    HttpScriptServices &services = *ctx->services();
    const JsValue options = (args.args != nullptr && args.argc > 0) ? args.args[0] : JsValue::make_undefined();

    auto target_opt = resolve_target(userdata);
    if (!target_opt) {
        co_return error_exn(*heap, "http.request: missing directive-bound target");
    }
    if (auto url_err = url_field_error("http.request", *heap, options)) {
        co_return error_exn(*heap, *url_err);
    }
    const std::chrono::milliseconds timeout = resolve_timeout(*heap, options);

    auto acq = co_await acquire_and_connect(services, *target_opt, timeout);
    if (!acq) {
        co_return error_exn(*heap, "http.request: acquire upstream connection failed");
    }
    AcquiredConnection &ac = *acq;

    fiber::http::ClientHttp1Exchange upstream(*ac.conn, ctx->exchange().pool());
    fiber::http::HttpHeaders req_headers(ctx->exchange().pool());
    apply_options_headers(*heap, options, req_headers);

    std::string body_bytes;
    bool end_stream = true;
    const fiber::http::HttpBodySpec body_spec = build_request_body(*heap, options, req_headers, body_bytes, end_stream);

    const std::string method_str = std::string(field_string(*heap, options, "method", 6));
    const fiber::http::HttpMethod method = method_str.empty()
                                                   ? fiber::http::HttpMethod::Get
                                                   : resolve_http_method(method_str, fiber::http::HttpMethod::Get);

    std::string target = build_request_target(*heap, options, "/");

    fiber::http::Http1RequestHead head;
    head.method = method;
    head.target = target;
    head.headers = &req_headers;
    head.body = body_spec;

    auto send_result = co_await upstream.send_header(head, end_stream, timeout);
    if (!send_result) {
        co_return error_exn(*heap, "http.request: send header failed");
    }
    if (!end_stream) {
        auto write_result = co_await upstream.write_all(reinterpret_cast<const std::uint8_t *>(body_bytes.data()),
                                                        body_bytes.size(), true, timeout);
        if (!write_result) {
            co_return error_exn(*heap, "http.request: write body failed");
        }
    }

    const fiber::http::Http1ResponseHead *resp_head = nullptr;
    for (;;) {
        auto read_result = co_await upstream.read_header(timeout);
        if (!read_result) {
            co_return error_exn(*heap, "http.request: read header failed");
        }
        if (!(*read_result)->is_informational()) {
            resp_head = *read_result;
            break;
        }
    }

    const bool include_headers = field_bool(*heap, options, "includeHeaders", 14, false);

    std::string body_out;
    auto body_read = co_await read_full_response_body(upstream, body_out, timeout);
    if (!body_read) {
        co_return error_exn(*heap, "http.request: read body failed");
    }

    // Build {status:int, headers?:object, body:binary}.
    GcHeap::LocalMark mark(*heap);
    ValueHandle obj_root = heap->local_value();
    if (!obj_root) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
    *obj_root = JsValue::make_object(*heap, 4);
    if (js_value_type(*obj_root) != JsNodeType::Object) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }

    if (!gc_put_value(*heap, obj_root, "status", 6, JsValue::make_integer(resp_head->status_code))) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }

    if (include_headers) {
        GcHeap::LocalMark m2(*heap);
        ValueHandle hs = heap->local_value();
        if (!hs) {
            co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
        *hs = JsValue::make_object(*heap, 8);
        if (js_value_type(*hs) != JsNodeType::Object) {
            co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
        for (const fiber::http::HttpHeaders::HeaderField &field: resp_head->headers) {
            if (field.name_len == 0) {
                continue;
            }
            gc_put_string(*heap, hs, field.name_view().data(), field.name_view().size(), field.value_view());
        }
        if (!gc_put_value(*heap, obj_root, "headers", 7, *hs)) {
            co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
        }
    }

    if (!gc_put_binary(*heap, obj_root, "body", 4, reinterpret_cast<const std::uint8_t *>(body_out.data()),
                       body_out.size())) {
        co_return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }

    co_return AbiResult::success(*obj_root);
}

// ---- http.proxyPass(options) -> upstream status int ----

AsyncTask http_proxy_pass_fn(void *userdata, const Library::HostCallFrame &frame, Library::Arguments args) noexcept {
    auto *ctx = ctx_of(frame);
    GcHeap *heap = &frame.runtime;
    if (ctx == nullptr || heap == nullptr || ctx->services() == nullptr) {
        co_return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    HttpScriptServices &services = *ctx->services();
    fiber::http::HttpExchange &exchange = ctx->exchange();
    const JsValue options = (args.args != nullptr && args.argc > 0) ? args.args[0] : JsValue::make_undefined();

    const bool websocket_requested = field_bool(*heap, options, "websocket", 9, false);
    WebSocketHandshake websocket;
    if (websocket_requested) {
        websocket.downstream = detect_websocket_downstream(exchange);
        if (!websocket.active()) {
            co_return error_exn(*heap, "http.proxyPass: websocket=true requires a WebSocket downstream request");
        }
        if (!prepare_websocket_handshake(websocket)) {
            co_return error_exn(*heap, "http.proxyPass: prepare websocket handshake failed");
        }
    }

    auto target_opt = resolve_target(userdata);
    if (!target_opt) {
        co_return error_exn(*heap, "http.proxyPass: missing directive-bound target");
    }
    if (auto url_err = url_field_error("http.proxyPass", *heap, options)) {
        co_return error_exn(*heap, *url_err);
    }
    const std::chrono::milliseconds timeout = resolve_timeout(*heap, options);

    fiber::http::HttpHeaders req_headers(exchange.pool());
    // Copy inbound request headers (hop-by-hop filtered), then apply options.headers overrides.
    for (const fiber::http::HttpHeaders::HeaderField &field: exchange.request_headers()) {
        if (field.name_len == 0 || is_request_framing_header(field) ||
            should_skip_hop_by_hop_header(exchange.request_headers(), field)) {
            continue;
        }
        req_headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }
    apply_options_headers(*heap, options, req_headers);
    remove_request_framing_headers(req_headers);
    if (websocket.active() && !prepare_upstream_websocket_headers(exchange, websocket, req_headers)) {
        co_return error_exn(*heap, "http.proxyPass: invalid websocket handshake headers");
    }

    const std::string method_str = std::string(field_string(*heap, options, "method", 6));
    if (websocket.active() && !method_str.empty() &&
        resolve_http_method(method_str, fiber::http::HttpMethod::Unknown) != fiber::http::HttpMethod::Get) {
        co_return error_exn(*heap, "http.proxyPass: websocket method must be GET");
    }
    const fiber::http::HttpMethod method =
            websocket.active()
                    ? fiber::http::HttpMethod::Get
                    : (method_str.empty() ? exchange.method() : resolve_http_method(method_str, exchange.method()));

    std::string target;
    {
        std::string_view url_sv = field_string(*heap, options, "url", 3);
        if (!url_sv.empty()) {
            // `url` is the full request path?query; it overrides path/query.
            target.assign(url_sv);
        } else {
            std::string_view path = field_string(*heap, options, "path", 4);
            if (path.empty()) {
                path = exchange.uri().path;
            }
            target.assign(path);
            JsValue qv = get_field(*heap, options, "query", 5);
            std::string_view qsv;
            if (string_utf8_view(qv, qsv) && !qsv.empty()) {
                target.push_back('?');
                target.append(qsv);
            } else if (js_value_type(qv) == JsNodeType::Object) {
                std::string qs;
                object_pairs_to_form(*heap, qv, qs);
                if (!qs.empty()) {
                    target.push_back('?');
                    target.append(qs);
                }
            } else if (!exchange.uri().query.empty()) {
                target.push_back('?');
                target.append(exchange.uri().query);
            }
        }
    }

    bool req_end_stream = true;
    const fiber::http::HttpBodySpec req_body =
            websocket.active() ? fiber::http::HttpBodySpec::None() : detect_request_body(exchange, req_end_stream);
    if (websocket.active()) {
        req_end_stream = true;
    }

    fiber::http::Http1RequestHead head;
    head.method = method;
    head.target = target;
    head.headers = &req_headers;
    head.body = req_body;

    auto acq = co_await acquire_and_connect(services, *target_opt, timeout);
    if (!acq) {
        co_return error_exn(*heap, "http.proxyPass: acquire upstream connection failed");
    }
    AcquiredConnection &ac = *acq;
    fiber::http::ClientHttp1Exchange upstream(*ac.conn, exchange.pool());

    auto send_result = co_await upstream.send_header(head, req_end_stream, timeout);
    if (!send_result) {
        co_return error_exn(*heap, "http.proxyPass: send request header failed");
    }
    if (!req_end_stream) {
        for (;;) {
            auto body_result = co_await exchange.read_body(kBodyChunkSize);
            if (!body_result) {
                co_return error_exn(*heap, "http.proxyPass: read request body failed");
            }
            const bool last = body_result->complete();
            auto write_result = co_await upstream.write_all(std::move(*body_result), timeout);
            if (!write_result) {
                co_return error_exn(*heap, "http.proxyPass: write request body failed");
            }
            if (last) {
                break;
            }
        }
    }

    const fiber::http::Http1ResponseHead *resp_head = nullptr;
    for (;;) {
        auto read_result = co_await upstream.read_header(timeout);
        if (!read_result) {
            co_return error_exn(*heap, "http.proxyPass: read response header failed");
        }
        if ((*read_result)->status_code == 101 || !(*read_result)->is_informational()) {
            resp_head = *read_result;
            break;
        }
    }

    if (resp_head->status_code == 101) {
        if (!valid_websocket_upgrade_response(*resp_head, websocket)) {
            co_return error_exn(*heap, "http.proxyPass: invalid websocket upgrade response");
        }
        auto switch_result = upstream.switch_to_raw_stream();
        if (!switch_result) {
            co_return error_exn(*heap, "http.proxyPass: switch websocket stream failed");
        }

        fiber::http::HttpHeaders resp_headers(exchange.pool());
        build_downstream_websocket_headers(*resp_head, resp_headers, websocket);
        apply_options_response_headers(*heap, options, resp_headers);
        finalize_downstream_websocket_headers(resp_headers, websocket);

        auto response_header_result = co_await exchange.send_header({
                .kind = fiber::http::OutgoingHeaderKind::Final,
                .status_code = websocket.extended_connect() ? 200 : 101,
                .reason = websocket.extended_connect() ? std::string_view{} : resp_head->reason,
                .headers = &resp_headers,
                .body = fiber::http::HttpBodySpec::Stream(),
                .connection_mode = fiber::http::ResponseConnectionMode::Auto,
                .end_stream = false,
        });
        if (!response_header_result) {
            (void) upstream.abort(response_header_result.error());
            co_return error_exn(*heap, "http.proxyPass: send websocket response header failed");
        }
        ctx->mark_response_sent();

        co_await relay_websocket_tunnel(exchange, upstream, timeout, timeout);
        co_return AbiResult::success(JsValue::make_integer(resp_head->status_code));
    }

    // Build downstream response headers (hop-by-hop filtered) + responseHeaders overrides.
    fiber::http::HttpHeaders resp_headers(exchange.pool());
    for (const fiber::http::HttpHeaders::HeaderField &field: resp_head->headers) {
        if (field.name_len == 0 || should_skip_hop_by_hop_header(resp_head->headers, field)) {
            continue;
        }
        resp_headers.add_view(field.name_view(), field.value_view(), field.lowcase_name, field.name_hash);
    }
    apply_options_response_headers(*heap, options, resp_headers);

    const bool no_body = response_has_no_body(method, resp_head->status_code);
    std::size_t response_content_length = 0;
    const bool has_content_length =
            !no_body &&
            parse_decimal(resp_head->headers.get("content-length", kContentLengthHash), response_content_length);
    const fiber::http::HttpBodySpec response_body =
            no_body ? fiber::http::HttpBodySpec::None()
                    : (has_content_length ? fiber::http::HttpBodySpec::ContentLength(response_content_length)
                                          : fiber::http::HttpBodySpec::Auto());

    const bool flush = field_bool(*heap, options, "flush", 5, false);

    auto response_header_result = co_await exchange.send_header({
            .kind = fiber::http::OutgoingHeaderKind::Final,
            .status_code = resp_head->status_code,
            .reason = resp_head->reason,
            .headers = &resp_headers,
            .body = response_body,
            .connection_mode = fiber::http::ResponseConnectionMode::Auto,
            .end_stream = no_body || (has_content_length && response_content_length == 0),
    });
    if (!response_header_result) {
        co_return error_exn(*heap, "http.proxyPass: send response header failed");
    }
    ctx->mark_response_sent();

    if (no_body) {
        (void) co_await upstream.discard_response_body(timeout);
        co_return AbiResult::success(JsValue::make_integer(resp_head->status_code));
    }
    if (has_content_length && response_content_length == 0) {
        co_return AbiResult::success(JsValue::make_integer(resp_head->status_code));
    }

    const fiber::http::HttpBodyPipeOptions pipe_options{
            .buffer_size = fiber::http::kDefaultBodyPipeBufferSize,
            .low_water = flush ? fiber::http::kUnbufferedBodyPipeLowWater : fiber::http::kDefaultBodyPipeLowWater,
            .read_timeout = timeout,
            .write_timeout = std::chrono::milliseconds::max(),
    };
    auto pipe_result = co_await fiber::http::pipe_http_body(
            fiber::http::make_http_body_pipe_reader(upstream), fiber::http::make_http_body_pipe_writer(exchange),
            fiber::event::EventLoop::current().io_buf_node_pool(), pipe_options);
    if (!pipe_result) {
        if (pipe_result.error().phase == fiber::http::HttpBodyPipePhase::Read) {
            co_return error_exn(*heap, "http.proxyPass: read response body failed");
        }
        if (pipe_result.error().phase == fiber::http::HttpBodyPipePhase::Write) {
            co_return error_exn(*heap, "http.proxyPass: write response body failed");
        }
        co_return error_exn(*heap, "http.proxyPass: invalid response body stream");
    }
    co_return AbiResult::success(JsValue::make_integer(resp_head->status_code));
}

// ---- HttpDirectiveDef ----

HttpDirectiveDef::HttpDirectiveDef(HttpTargetSpec target) noexcept : target_(std::move(target)) {
    request_callable_.kind = Library::HostCallable::Kind::AsyncFunction;
    request_callable_.async_function = &http_request_fn;
    request_callable_.userdata = this;
    request_callable_.debug_name = "http.request";
    proxy_pass_callable_.kind = Library::HostCallable::Kind::AsyncFunction;
    proxy_pass_callable_.async_function = &http_proxy_pass_fn;
    proxy_pass_callable_.userdata = this;
    proxy_pass_callable_.debug_name = "http.proxyPass";
}

Library::FunctionMatchResult HttpDirectiveDef::resolve_func(std::string_view /*directive*/,
                                                            std::string_view /*function*/,
                                                            const Library::FunctionMatchRequest & /*request*/,
                                                            const Library & /*library*/) const {
    return Library::FunctionMatchResult::not_found();
}

Library::FunctionMatchResult HttpDirectiveDef::resolve_async_func(std::string_view /*directive*/,
                                                                  std::string_view function,
                                                                  const Library::FunctionMatchRequest &request,
                                                                  const Library & /*library*/) const {
    const Library::HostCallable *callable = nullptr;
    if (function == "request") {
        callable = &request_callable_;
    } else if (function == "proxyPass") {
        callable = &proxy_pass_callable_;
    } else {
        return Library::FunctionMatchResult::not_found();
    }
    if (request.has_spread || request.known_argc > 1) {
        return Library::FunctionMatchResult::arity_mismatch();
    }
    Library::FunctionSignature sig{};
    sig.required_argc = static_cast<std::uint16_t>(request.known_argc);
    sig.fixed_argc = static_cast<std::uint16_t>(request.known_argc);
    sig.variadic = false;
    return Library::FunctionMatchResult::found(callable, sig, nullptr, 0);
}

} // namespace fiber::http_script
