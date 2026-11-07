#ifndef FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
#define FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "../http/HttpExchange.h"
#include "../http/HttpHeaders.h"
#include "../http/HttpResponseWriter.h"
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
//   2. Memoized, on-demand resolution of script constants ($path/$query/$header/$cookie).
//      Compiled constants arrive from their ConstPackage as (type, normalized name, hash)
//      and are resolved against request state at first access, then cached. The ctx holds
//      no per-package state, so scripts compiled against different packages — including a
//      new configuration generation mid-request — share one ctx.
//   3. A response state machine: response headers accumulate in pending_headers_ until a
//      send/write flushes them through the host-provided HttpResponseWriter (header_sent_
//      guards against post-send mutation).
class ScriptExchangeCtx {
public:
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap) noexcept;
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap,
                      ScriptConnectionInfo connection) noexcept;
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap, ScriptConnectionInfo connection,
                      ScriptRequestBody request_body) noexcept;
    ScriptExchangeCtx(fiber::http::HttpExchange &exchange, fiber::script::GcHeap &heap, ScriptConnectionInfo connection,
                      ScriptRequestBody request_body, fiber::http::HttpResponseWriter response_writer) noexcept;
    ~ScriptExchangeCtx() = default;

    ScriptExchangeCtx(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx &operator=(const ScriptExchangeCtx &) = delete;
    ScriptExchangeCtx(ScriptExchangeCtx &&) = delete;
    ScriptExchangeCtx &operator=(ScriptExchangeCtx &&) = delete;

    [[nodiscard]] fiber::http::HttpExchange &exchange() const noexcept { return exchange_; }
    [[nodiscard]] fiber::http::HttpResponseWriter &response_writer() noexcept { return response_writer_; }
    [[nodiscard]] const fiber::http::HttpResponseWriter &response_writer() const noexcept { return response_writer_; }
    // Replace the response sink before the script emits headers or body bytes. The
    // replacement remains request-local and is also used by http.proxyPass.
    void set_response_writer(fiber::http::HttpResponseWriter response_writer) noexcept {
        response_writer_ = response_writer;
    }
    [[nodiscard]] fiber::script::GcHeap &heap() const noexcept { return heap_; }
    [[nodiscard]] ScriptRequestBody request_body() const noexcept { return request_body_; }
    // The body is a one-shot stream: set as soon as a consumer starts on it, whatever the
    // outcome, so a later consumer fails instead of seeing the tail of a half-read body.
    [[nodiscard]] bool request_body_consumed() const noexcept { return request_body_consumed_; }
    void mark_request_body_consumed() noexcept { request_body_consumed_ = true; }

    // App-provided upstream-connection services (global pool + DNS). Set per request by the host
    // before the script runs; null => http.request / http.proxyPass fail with InvalidState.
    void set_services(HttpScriptServices *services) noexcept { services_ = services; }
    [[nodiscard]] HttpScriptServices *services() const noexcept { return services_; }

    // ---- Request views (lazy, cached, GC-rooted) ----
    // Each returns the cached JsValue (Object) or, on allocation failure, Undefined.
    [[nodiscard]] fiber::script::JsValue query() noexcept;
    [[nodiscard]] fiber::script::JsValue headers() noexcept;
    [[nodiscard]] fiber::script::JsValue cookies() noexcept;

    // Route captures for $path.<name>, injected by the host after route match and before
    // scripts run. Names and values borrow the matcher text / request path buffer; the span
    // itself must outlive every script execution against this ctx.
    //
    // Rebinding swaps the captures (route re-matching across candidates), so memoized Path
    // constants must not survive: the next candidate's $path.* would read the previous
    // candidate's values. Unlinking single entries would break the linear-probe chains, so
    // the whole memo resets here; every entry re-derives from request state on demand.
    void set_path_vars(std::span<const std::pair<std::string_view, std::string_view>> vars) noexcept {
        path_vars_ = vars;
        if (constant_cache_ != nullptr) {
            for (std::size_t i = 0; i < constant_cache_capacity_; ++i) {
                constant_cache_[i] = ConstantEntry{};
            }
            constant_cache_size_ = 0;
        }
    }

    // Runtime entry for ConstPackage-compiled constants. `name` is the package-normalized
    // key and `hash` its const_name_hash, both precomputed at package build. First access
    // resolves the value from request state and memoizes it; later accesses are a single
    // probe. Absent values resolve to Null; only allocation failure aborts (OutOfMemory).
    //
    // `name` must point into the caller's ConstPackage storage and is borrowed by the memo
    // for the ctx lifetime: a compiled script and its ConstPackage must outlive every
    // ScriptExchangeCtx the script executes against.
    [[nodiscard]] fiber::script::AbiResult resolve_constant(ConstType type, std::string_view name,
                                                            std::uint64_t hash) noexcept;

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

    // Encodes body as JSON and sends it with Content-Type application/json. A short body goes out
    // like write_raw_bytes. A longer one is sent as it is encoded, with no Content-Length: if
    // encoding then fails, the response is aborted and the client sees a broken body. body and
    // everything it reaches must stay unchanged until this completes. When the response has no
    // content, as for HEAD, a longer body is not encoded past the point of knowing it is longer.
    fiber::async::Task<fiber::common::IoResult<void>> write_json(int status,
                                                                 const fiber::script::JsValue &body) noexcept;
    // Flush the pending headers (status + body of known length) then write the body in a
    // single end-of-stream chunk. content-type is the caller's responsibility (set via the
    // set/add helpers above). Empty-body variants send a terminating empty chunk.
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

    // Open-addressing memo of resolved constants (power-of-two capacity, linear probing,
    // grown at 3/4 load so at least one slot stays empty and probes terminate). Entries
    // hold only immediate JsValues — native strings backed by the request buffers or the
    // exchange pool, or Null for a confirmed miss — so no GC rooting is needed.
    //
    // `name` borrows the normalized key from the owning ConstPackage's Ref and keeps
    // borrowing it for the ctx lifetime. Contract: a compiled script and its ConstPackage
    // must OUTLIVE every ScriptExchangeCtx the script executes against — the script's own
    // compiled userdata points into the same storage, so any caller keeping the script
    // runnable already satisfies this. name.data() == nullptr marks an empty slot.
    struct ConstantEntry {
        std::uint64_t hash = 0;
        std::string_view name;
        ConstType type = ConstType::Count;
        fiber::script::JsValue value{};
    };

    // Builds the cached object into its root slot on first access. Returns the cached
    // JsValue (or Undefined on failure). Idempotent: a failed build leaves the slot empty
    // so the next call retries.
    fiber::script::JsValue build_query() noexcept;
    fiber::script::JsValue build_headers() noexcept;
    fiber::script::JsValue build_cookies() noexcept;
    fiber::script::AbiResult remote_address_constant() noexcept;

    [[nodiscard]] const ConstantEntry *cache_find(std::uint64_t hash, ConstType type,
                                                  std::string_view name) const noexcept;
    [[nodiscard]] bool cache_insert(std::uint64_t hash, ConstType type, std::string_view name,
                                    fiber::script::JsValue value) noexcept;
    // Resolves a constant directly from request state. Returns false only on allocation
    // failure (query decode copy); a confirmed miss yields Null in out. Duplicate-key
    // semantics per type: Header/Cookie first occurrence wins, Query/Path last wins —
    // matching the previous prepare-time slot fill.
    [[nodiscard]] bool resolve_from_request(ConstType type, std::string_view name,
                                            fiber::script::JsValue &out) noexcept;

    [[nodiscard]] fiber::http::OutgoingHeaderBlockView final_header(int status,
                                                                    fiber::http::HttpBodySpec body) const noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> send_final_with_body(int status, std::size_t content_length,
                                                                           const std::uint8_t *data) noexcept;
    // Chain-body variant: encodes straight into the transport buffer (no staging copy);
    // end-of-stream comes from body.complete(), set by the implementation before write_all.
    fiber::async::Task<fiber::common::IoResult<void>> send_final_with_body(int status,
                                                                           fiber::mem::IoBufChain body) noexcept;

    fiber::http::HttpExchange &exchange_;
    fiber::http::HttpResponseWriter response_writer_;
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

    std::span<const std::pair<std::string_view, std::string_view>> path_vars_{};
    ConstantEntry *constant_cache_ = nullptr; // exchange pool; null until first insert
    std::size_t constant_cache_capacity_ = 0; // power of two
    std::size_t constant_cache_size_ = 0;

    fiber::http::HttpHeaders pending_headers_;
    bool header_sent_ = false;
    bool request_body_consumed_ = false;
};

} // namespace fiber::http_script

#endif // FIBER_HTTP_SCRIPT_EXCHANGE_CTX_H
