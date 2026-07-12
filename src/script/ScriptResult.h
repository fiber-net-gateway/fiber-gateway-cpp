#ifndef FIBER_SCRIPT_SCRIPT_RESULT_H
#define FIBER_SCRIPT_SCRIPT_RESULT_H

#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>

#include "../common/Assert.h"
#include "JsValue.h"

namespace fiber::script {

// Kind for the host-call ABI result (AbiResult). The script-exec return type ScriptResult
// below has its own ScriptResultKind {Value, Void, Exception, Abort}.
enum class AbiResultKind : std::uint8_t {
    Success,
    Exception,
    Abort,
};

enum class CallResult : std::uint8_t {
    Success,
    Exception,
    Abort,
};

enum class ScriptAbortReason : std::uint8_t {
    None,
    OutOfMemory,
    InvalidState,
    InvalidOpcode,
    HostFault,
    Timeout,
    Cancelled,
    Internal,
};

// Display name for a script abort reason ("OutOfMemory", "Timeout", ...). Used by hosts that
// surface a script abort as a JSON error body. Returns a stable ASCII identifier; never null.
std::string_view abort_reason_name(ScriptAbortReason reason) noexcept;

struct ScriptAbort {
    ScriptAbortReason reason = ScriptAbortReason::None;
    std::int64_t position = -1;
};

struct ScriptStatus {
    constexpr ScriptStatus() noexcept = default;

    static ScriptStatus success() noexcept {
        ScriptStatus status;
        status.kind = AbiResultKind::Success;
        return status;
    }

    static ScriptStatus exception() noexcept {
        ScriptStatus status;
        status.kind = AbiResultKind::Exception;
        return status;
    }

    static ScriptStatus abort(ScriptAbortReason reason, std::int64_t position = -1) noexcept {
        ScriptStatus status;
        status.kind = AbiResultKind::Abort;
        status.abort_payload = ScriptAbort{reason, position};
        return status;
    }

    [[nodiscard]] bool is_success() const noexcept { return kind == AbiResultKind::Success; }

    [[nodiscard]] bool is_exception() const noexcept { return kind == AbiResultKind::Exception; }

    [[nodiscard]] bool is_abort() const noexcept { return kind == AbiResultKind::Abort; }

    [[nodiscard]] bool is_pending() const noexcept {
        return kind == AbiResultKind::Abort && abort_payload.reason == ScriptAbortReason::None;
    }

    [[nodiscard]] bool is_done() const noexcept { return !is_pending(); }

    [[nodiscard]] const ScriptAbort &abort() const noexcept {
        FIBER_ASSERT(is_abort());
        return abort_payload;
    }

    [[nodiscard]] bool has_value() const noexcept { return is_success(); }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    AbiResultKind kind = AbiResultKind::Abort;
    ScriptAbort abort_payload{};
};

union ResultPayload {
    constexpr ResultPayload() : value{} {}

    fiber::script::JsValue value;
    fiber::script::JsValue exception;
    ScriptAbort abort;
};

struct alignas(16) AbiResult {
    union Payload {
        constexpr Payload() : abort{} {}

        fiber::script::JsValue value;
        ScriptAbort abort;
    };

    constexpr AbiResult() noexcept = default;

    AbiResult(const fiber::script::JsValue &value) noexcept : kind(AbiResultKind::Success), payload{} {
        payload.value = value;
    }

    AbiResult(std::unexpected<fiber::script::JsValue> unexpected) noexcept : kind(AbiResultKind::Exception), payload{} {
        payload.value = unexpected.error();
    }

    static AbiResult success(const fiber::script::JsValue &value) noexcept {
        AbiResult result;
        result.kind = AbiResultKind::Success;
        result.payload.value = value;
        return result;
    }

    static AbiResult exception(const fiber::script::JsValue &value) noexcept {
        AbiResult result;
        result.kind = AbiResultKind::Exception;
        result.payload.value = value;
        return result;
    }

    static AbiResult abort(ScriptAbortReason reason, std::int64_t position = -1) noexcept {
        AbiResult result;
        result.kind = AbiResultKind::Abort;
        result.payload.abort = ScriptAbort{reason, position};
        return result;
    }

    [[nodiscard]] bool is_success() const noexcept { return kind == AbiResultKind::Success; }

    [[nodiscard]] bool is_exception() const noexcept { return kind == AbiResultKind::Exception; }

    [[nodiscard]] bool is_abort() const noexcept { return kind == AbiResultKind::Abort; }

    [[nodiscard]] bool is_pending() const noexcept {
        return kind == AbiResultKind::Abort && payload.abort.reason == ScriptAbortReason::None;
    }

    [[nodiscard]] bool is_done() const noexcept { return !is_pending(); }

    [[nodiscard]] const fiber::script::JsValue &value() const noexcept {
        FIBER_ASSERT(is_success());
        return payload.value;
    }

    [[nodiscard]] const fiber::script::JsValue &exception() const noexcept {
        FIBER_ASSERT(is_exception());
        return payload.value;
    }

    [[nodiscard]] const ScriptAbort &abort() const noexcept {
        FIBER_ASSERT(is_abort());
        return payload.abort;
    }

    [[nodiscard]] bool has_value() const noexcept { return is_success(); }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const fiber::script::JsValue &error() const noexcept { return exception(); }

    AbiResultKind kind = AbiResultKind::Abort;
    Payload payload{};
};

static_assert(std::is_trivially_copyable_v<ScriptAbort>);
static_assert(std::is_trivially_copyable_v<ScriptStatus>);
static_assert(std::is_trivially_copyable_v<ResultPayload>);
static_assert(std::is_trivially_copyable_v<AbiResult>);

// Script-exec return type. Distinct from AbiResult (the host-call ABI): a script that ends
// without producing a value (bare `return;` or falling off the end) yields Void rather than
// Value(undefined). Value and Void both mean "ended correctly"; Exception/Abort are failures.
// Void exists only here -- host functions still signal "no value" as AbiResult::success(undefined).
enum class ScriptResultKind : std::uint8_t {
    Value,
    Void,
    Exception,
    Abort,
};

struct alignas(16) ScriptResult {
    union Payload {
        constexpr Payload() : abort{} {}

        fiber::script::JsValue value;
        ScriptAbort abort;
    };

    constexpr ScriptResult() noexcept = default;

    ScriptResult(const fiber::script::JsValue &value) noexcept : kind(ScriptResultKind::Value), payload{} {
        payload.value = value;
    }

    ScriptResult(std::unexpected<fiber::script::JsValue> unexpected) noexcept :
        kind(ScriptResultKind::Exception), payload{} {
        payload.value = unexpected.error();
    }

    static ScriptResult value(const fiber::script::JsValue &v) noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Value;
        result.payload.value = v;
        return result;
    }

    static ScriptResult void_() noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Void;
        return result;
    }

    static ScriptResult exception(const fiber::script::JsValue &v) noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Exception;
        result.payload.value = v;
        return result;
    }

    static ScriptResult abort(ScriptAbortReason reason, std::int64_t position = -1) noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Abort;
        result.payload.abort = ScriptAbort{reason, position};
        return result;
    }

    [[nodiscard]] bool is_value() const noexcept { return kind == ScriptResultKind::Value; }

    [[nodiscard]] bool is_void() const noexcept { return kind == ScriptResultKind::Void; }

    [[nodiscard]] bool is_exception() const noexcept { return kind == ScriptResultKind::Exception; }

    [[nodiscard]] bool is_abort() const noexcept { return kind == ScriptResultKind::Abort; }

    // Value and Void are both "ended correctly".
    [[nodiscard]] bool is_success() const noexcept { return is_value() || is_void(); }

    [[nodiscard]] bool is_pending() const noexcept {
        return kind == ScriptResultKind::Abort && payload.abort.reason == ScriptAbortReason::None;
    }

    [[nodiscard]] bool is_done() const noexcept { return !is_pending(); }

    [[nodiscard]] const fiber::script::JsValue &value() const noexcept {
        FIBER_ASSERT(is_value());
        return payload.value;
    }

    [[nodiscard]] const fiber::script::JsValue &exception() const noexcept {
        FIBER_ASSERT(is_exception());
        return payload.value;
    }

    [[nodiscard]] const ScriptAbort &abort() const noexcept {
        FIBER_ASSERT(is_abort());
        return payload.abort;
    }

    // expected-style: only Value carries a value (Void does not).
    [[nodiscard]] bool has_value() const noexcept { return is_value(); }

    [[nodiscard]] explicit operator bool() const noexcept { return is_success(); }

    [[nodiscard]] const fiber::script::JsValue &error() const noexcept { return exception(); }

    ScriptResultKind kind = ScriptResultKind::Abort;
    Payload payload{};
};

static_assert(std::is_trivially_copyable_v<ScriptResult>);

// ResultPayload mutators — the single way op functions write their outcome. Ops never carry a
// position (the dispatch layer recovers it from the current pc_), so set_abort takes only a reason.
inline CallResult set_value(ResultPayload &result, const fiber::script::JsValue &value) noexcept {
    result.value = value;
    return CallResult::Success;
}

inline CallResult set_undefined(ResultPayload &result) noexcept {
    result.value = fiber::script::JsValue::make_undefined();
    return CallResult::Success;
}

inline CallResult set_exception(ResultPayload &result, fiber::script::ExceptionKind kind) noexcept {
    result.exception = fiber::script::JsValue::make_exception(kind);
    return CallResult::Exception;
}

inline CallResult set_abort(ResultPayload &result, ScriptAbortReason reason) noexcept {
    result.abort = ScriptAbort{reason, -1};
    return CallResult::Abort;
}

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_RESULT_H
