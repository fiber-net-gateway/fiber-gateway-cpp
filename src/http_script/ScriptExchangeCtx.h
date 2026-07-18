#ifndef FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
#define FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../http/HttpExchange.h"
#include "../http/HttpHeaders.h"
#include "../script/JsGc.h"
#include "../script/JsValue.h"
#include "../script/ScriptResult.h"

#include "HttpScriptServices.h"

namespace fiber::http_script {

struct ScriptConnectionInfo {
    std::string_view scheme;
    bool tls = false;
};

// Per-request script attach payload. Bound to one HttpExchange and one script GcHeap for
// the lifetime of a script invocation; the host constructs it and passes `&ctx` as the
// `attach` argument to Script::exec_async/exec_sync, so req.*/resp.* host functions can
// recover it via `static_cast<ScriptExchangeCtx *>(frame.attach)`.
//
// It centralizes two concerns that the C++ HttpExchange does not expose directly (unlike
// Java's HttpExchange):
//   1. Lazy, cached JsValue views of the request query/headers/cookies, held across calls
//      in persistent GC root slots (GcHeap::global_value), mirroring Java ReqFunc.Ctx
//      stored via HttpExchange.Attr.
//   2. A response state machine: response headers accumulate in pending_headers_ until a
//      send/write flushes them via HttpExchange::send_header + write_body (header_sent_
//      guards against post-send mutation).
class ScriptExchangeCtx {
public:
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap) noexcept;
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                      ScriptConnectionInfo connection) noexcept;
    ~ScriptExchangeCtx() = default;

    ScriptExchangeCtx(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx &operator=(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx(ScriptExchangeCtx &&) = delete;
    ScriptExchangeCtx &operator=(ScriptExchangeCtx &&) = delete;

    [[nodiscard]] fiber::http::HttpExchange &exchange() const noexcept { return exchange_; }
    [[nodiscard]] fiber::script::GcHeap &heap() const noexcept { return heap_; }

    // App-provided upstream-connection services (global pool + DNS). Set per request by the host
    // before the script runs; null => http.request / http.proxyPass fail with InvalidState.
    void set_services(HttpScriptServices *services) noexcept { services_ = services; }
    [[nodiscard]] HttpScriptServices *services() const noexcept { return services_; }

    // ---- Request views (lazy, cached, GC-rooted) ----
    // Each returns the cached JsValue (Object) or, on allocation failure, Undefined.
    [[nodiscard]] fiber::script::JsValue query() noexcept;
    [[nodiscard]] fiber::script::JsValue headers() noexcept;
    [[nodiscard]] fiber::script::JsValue cookies() noexcept;

    // ---- Route-variable constant accessors ($namespace.key) ----
    // Back the $path/$query/$header/$cookie/$req constants resolved by RouteScriptLibrary.
    // Each returns a String JsValue, or Undefined when the named value is absent.

    // Path variables captured by the route matcher for this request. name/value pairs borrow
    // the matcher text and request path buffer, so the caller must keep them alive for the
    // duration of the script invocation.
    void set_path_vars(const std::vector<std::pair<std::string_view, std::string_view>> &path_vars) noexcept {
        path_vars_ = path_vars;
    }
    [[nodiscard]] fiber::script::AbiResult path_var(fiber::script::GcHeap &heap, std::string_view name) const noexcept;
    [[nodiscard]] fiber::script::AbiResult query_var(fiber::script::GcHeap &heap, std::string_view name) noexcept;
    // norm_key is already lowercased with '-' folded to '_' (RouteScriptLibrary normalizes at
    // compile time); header/cookie names are matched under the same rule.
    [[nodiscard]] fiber::script::AbiResult header_var(fiber::script::GcHeap &heap,
                                                      std::string_view norm_key) const noexcept;
    [[nodiscard]] fiber::script::AbiResult cookie_var(fiber::script::GcHeap &heap,
                                                      std::string_view norm_key) const noexcept;
    // field is one of "uri" / "method" / "path" / "query".
    [[nodiscard]] fiber::script::AbiResult req_field(fiber::script::GcHeap &heap,
                                                     std::string_view field) const noexcept;
    // field is one of remote_addr / remote_port / http_version / scheme / tls.
    [[nodiscard]] fiber::script::AbiResult conn_field(fiber::script::GcHeap &heap,
                                                      std::string_view field) const noexcept;

    // Looks up key on a GC object (the cached query/headers/cookies views). Returns Undefined
    // when the object is absent or the key is not present; aborts only on allocation failure.
    [[nodiscard]] static fiber::script::AbiResult
    lookup_property(fiber::script::GcHeap &heap, fiber::script::JsValue object, std::string_view key) noexcept;


    // ---- Response state machine ----
    // Accumulate response headers. No-ops once the header has been sent.
    void set_response_header(std::string_view name, std::string_view value) noexcept;
    void add_response_header(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] bool response_header_sent() const noexcept { return header_sent_; }

    // Mark the response as already sent (e.g. http.proxyPass writes the response directly to the
    // exchange, bypassing the ctx state machine). Prevents run_script from emitting a fallback 500.
    void mark_response_sent() noexcept { header_sent_ = true; }

    // Flush the pending headers (status + body of known length) then write the body in a
    // single end-of-stream chunk. content-type is the caller's responsibility (set via the
    // set/add helpers above). Empty-body variants send a terminating empty chunk.
    fiber::async::Task<fiber::common::IoResult<void>> write_json(int status,
                                                                 const fiber::script::JsValue &body) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> write_raw_bytes(int status, const std::uint8_t *data,
                                                                      std::size_t len) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> write_empty(int status) noexcept;

    // Synthesize a JSON error body {"error":"<error_name>"} for hosts that surface a tagged
    // exception (no heap GcException) or a script abort as a 500. error_name must be a stable
    // ASCII identifier (no escaping). Does not touch the GC heap, so it is safe to call after
    // an out-of-memory abort.
    fiber::async::Task<fiber::common::IoResult<void>> write_error_json(int status,
                                                                       std::string_view error_name) noexcept;

private:
    // Builds the cached object into its root slot on first access. Returns the cached
    // JsValue (or Undefined on failure). Idempotent: a failed build leaves the slot empty
    // so the next call retries.
    fiber::script::JsValue build_query() noexcept;
    fiber::script::JsValue build_headers() noexcept;
    fiber::script::JsValue build_cookies() noexcept;

    fiber::async::Task<fiber::common::IoResult<void>> send_final_with_body(int status, std::size_t content_length,
                                                                           const std::uint8_t *data) noexcept;

    fiber::http::HttpExchange &exchange_;
    fiber::script::GcHeap &heap_;
    ScriptConnectionInfo connection_{};
    HttpScriptServices *services_ = nullptr;

    // Persistent GC root slots (GcHeap::global_value) holding the cached request views.
    // nullptr until first access / until alloc succeeds.
    fiber::script::ValueHandle query_root_{};
    fiber::script::ValueHandle headers_root_{};
    fiber::script::ValueHandle cookies_root_{};

    // Path variables captured by the route matcher for the current request (non-owning
    // views; populated by set_path_vars before the script runs).
    std::span<const std::pair<std::string_view, std::string_view>> path_vars_;

    fiber::http::HttpHeaders pending_headers_;
    bool header_sent_ = false;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
