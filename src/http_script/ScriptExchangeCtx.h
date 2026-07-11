#ifndef FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
#define FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H

#include <cstdint>
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

namespace fiber::http_script {

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
    ~ScriptExchangeCtx() = default;

    ScriptExchangeCtx(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx &operator=(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx(ScriptExchangeCtx &&) = delete;
    ScriptExchangeCtx &operator=(ScriptExchangeCtx &&) = delete;

    [[nodiscard]] fiber::http::HttpExchange &exchange() const noexcept { return exchange_; }
    [[nodiscard]] fiber::script::GcHeap &heap() const noexcept { return heap_; }

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
    [[nodiscard]] fiber::script::ScriptResult path_var(fiber::script::GcHeap &heap,
                                                       std::string_view name) const noexcept;
    [[nodiscard]] fiber::script::ScriptResult query_var(fiber::script::GcHeap &heap, std::string_view name) noexcept;
    // norm_key is already lowercased with '-' folded to '_' (RouteScriptLibrary normalizes at
    // compile time); header/cookie names are matched under the same rule.
    [[nodiscard]] fiber::script::ScriptResult header_var(fiber::script::GcHeap &heap,
                                                         std::string_view norm_key) const noexcept;
    [[nodiscard]] fiber::script::ScriptResult cookie_var(fiber::script::GcHeap &heap,
                                                         std::string_view norm_key) const noexcept;
    // field is one of "uri" / "method" / "path" / "query".
    [[nodiscard]] fiber::script::ScriptResult req_field(fiber::script::GcHeap &heap,
                                                        std::string_view field) const noexcept;

    // Looks up key on a GC object (the cached query/headers/cookies views). Returns Undefined
    // when the object is absent or the key is not present; aborts only on allocation failure.
    [[nodiscard]] static fiber::script::ScriptResult
    lookup_property(fiber::script::GcHeap &heap, fiber::script::JsValue object, std::string_view key) noexcept;


    // ---- Response state machine ----
    // Accumulate response headers. No-ops once the header has been sent.
    void set_response_header(std::string_view name, std::string_view value) noexcept;
    void add_response_header(std::string_view name, std::string_view value) noexcept;
    [[nodiscard]] bool response_header_sent() const noexcept { return header_sent_; }

    // Flush the pending headers (status + body of known length) then write the body in a
    // single end-of-stream chunk. content-type is the caller's responsibility (set via the
    // set/add helpers above). Empty-body variants send a terminating empty chunk.
    fiber::async::Task<fiber::common::IoResult<void>> write_json(int status,
                                                                 const fiber::script::JsValue &body) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> write_raw_bytes(int status, const std::uint8_t *data,
                                                                      std::size_t len) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> write_empty(int status) noexcept;

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

    // Persistent GC root slots (GcHeap::global_value) holding the cached request views.
    // nullptr until first access / until alloc succeeds.
    fiber::script::ValueHandle query_root_{};
    fiber::script::ValueHandle headers_root_{};
    fiber::script::ValueHandle cookies_root_{};

    // Path variables captured by the route matcher for the current request (non-owning
    // views; populated by set_path_vars before the script runs).
    std::vector<std::pair<std::string_view, std::string_view>> path_vars_;

    fiber::http::HttpHeaders pending_headers_;
    bool header_sent_ = false;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
