#ifndef FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
#define FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../http/HttpExchange.h"
#include "../http/HttpHeaders.h"
#include "../script/JsGc.h"
#include "../script/JsValue.h"
#include "../script/ScriptResult.h"

#include "ConstPackage.h"
#include "HttpScriptServices.h"
#include "ScriptRequestBody.h"

namespace fiber::http_script {

class ExchangeConstExtension;

struct ScriptConnectionInfo {
    std::string_view scheme;
    bool tls = false;
};

struct IndexedConstValue {
    ConstIndex index = kInvalidConstIndex;
    std::string_view value;
};

// Per-request script attach payload. Bound to one HttpExchange and one script GcHeap for
// the request lifetime and reused by serial script invocations. The host passes `&ctx` as
// the `attach` argument to Script::exec_async/exec_sync, so req.*/resp.* host functions can
// recover it via `static_cast<ScriptExchangeCtx *>(frame.attach)`.
//
// It centralizes three concerns that the C++ HttpExchange does not expose directly (unlike
// Java's HttpExchange):
//   1. Lazy, cached JsValue views of the request query/headers/cookies, held across calls
//      in persistent GC root slots (GcHeap::global_value), mirroring Java ReqFunc.Ctx
//      stored via HttpExchange.Attr.
//   2. An indexed dynamic-constant frame prepared from an immutable ConstPackage before scripts run.
//   3. A response state machine: response headers accumulate in pending_headers_ until a
//      send/write flushes them via HttpExchange::send_header + write_all (header_sent_
//      guards against post-send mutation).
class ScriptExchangeCtx {
public:
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap) noexcept;
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                      ScriptConnectionInfo connection) noexcept;
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap, ScriptConnectionInfo connection,
                      ScriptRequestBody request_body) noexcept;
    ~ScriptExchangeCtx() = default;

    ScriptExchangeCtx(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx &operator=(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx(ScriptExchangeCtx &&) = delete;
    ScriptExchangeCtx &operator=(ScriptExchangeCtx &&) = delete;

    [[nodiscard]] fiber::http::HttpExchange &exchange() const noexcept { return exchange_; }
    [[nodiscard]] fiber::script::GcHeap &heap() const noexcept { return heap_; }
    [[nodiscard]] ScriptRequestBody request_body() const noexcept { return request_body_; }

    // App-provided upstream-connection services (global pool + DNS). Set per request by the host
    // before the script runs; null => http.request / http.proxyPass fail with InvalidState.
    void set_services(HttpScriptServices *services) noexcept { services_ = services; }
    [[nodiscard]] HttpScriptServices *services() const noexcept { return services_; }

    // ---- Request views (lazy, cached, GC-rooted) ----
    // Each returns the cached JsValue (Object) or, on allocation failure, Undefined.
    [[nodiscard]] fiber::script::JsValue query() noexcept;
    [[nodiscard]] fiber::script::JsValue headers() noexcept;
    [[nodiscard]] fiber::script::JsValue cookies() noexcept;

    // Prepares the request-wide dynamic constant slots used by every compiled script bound
    // to package. The slot array contains only immediate or borrowed JsValues; decoded query
    // values are copied into the request pool before they are stored. Path variables are
    // bound separately once the route matcher has selected a candidate. Fixed $req/$conn
    // fields are read directly by ExchangeConstExtension and do not consume slots.
    [[nodiscard]] fiber::common::IoResult<void>
    prepare_constants(const ConstPackage &package, std::span<const IndexedConstValue> external_values = {}) noexcept;
    [[nodiscard]] bool bind_constant(ConstIndex index, std::string_view value) noexcept;
    [[nodiscard]] bool bind_constants(std::span<const IndexedConstValue> values) noexcept;
    [[nodiscard]] bool
    bind_path_constants(const ConstPackage &package,
                        std::span<const std::pair<std::string_view, std::string_view>> path_values) noexcept;
    void clear_constants(std::span<const ConstIndex> indices) noexcept;

    // Called by ConstPackage's compiled HostCallable. package_identity must match the
    // immutable package used by prepare_constants.
    [[nodiscard]] fiber::script::AbiResult constant(const void *package_identity, ConstIndex index) const noexcept;

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
    friend class ExchangeConstExtension;

    // Builds the cached object into its root slot on first access. Returns the cached
    // JsValue (or Undefined on failure). Idempotent: a failed build leaves the slot empty
    // so the next call retries.
    fiber::script::JsValue build_query() noexcept;
    fiber::script::JsValue build_headers() noexcept;
    fiber::script::JsValue build_cookies() noexcept;
    fiber::script::AbiResult remote_address_constant() noexcept;

    fiber::async::Task<fiber::common::IoResult<void>> send_final_with_body(int status, std::size_t content_length,
                                                                           const std::uint8_t *data) noexcept;

    fiber::http::HttpExchange &exchange_;
    fiber::script::GcHeap &heap_;
    ScriptConnectionInfo connection_{};
    ScriptRequestBody request_body_;
    HttpScriptServices *services_ = nullptr;

    // Persistent GC root slots (GcHeap::global_value) holding the cached request views.
    // nullptr until first access / until alloc succeeds.
    fiber::script::ValueHandle query_root_{};
    fiber::script::ValueHandle headers_root_{};
    fiber::script::ValueHandle cookies_root_{};
    // Undefined until first use. A successful conversion is a native string backed by
    // exchange.pool(); conversion failure is cached as Null, while OOM remains retryable.
    fiber::script::JsValue remote_addr_constant_{};

    const void *const_package_identity_ = nullptr;
    fiber::script::JsValue *constant_slots_ = nullptr;
    std::size_t constant_slot_count_ = 0;

    fiber::http::HttpHeaders pending_headers_;
    bool header_sent_ = false;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
