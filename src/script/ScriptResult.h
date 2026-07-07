#ifndef FIBER_SCRIPT_SCRIPT_RESULT_H
#define FIBER_SCRIPT_SCRIPT_RESULT_H

#include <cstdint>
#include <expected>
#include <type_traits>

#include "../common/Assert.h"
#include "json/JsNode.h"

namespace fiber::script {

enum class ScriptResultKind : std::uint8_t {
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

struct ScriptAbort {
    ScriptAbortReason reason = ScriptAbortReason::None;
    std::int64_t position = -1;
};

struct ScriptStatus {
    constexpr ScriptStatus() noexcept = default;

    static ScriptStatus success() noexcept {
        ScriptStatus status;
        status.kind = ScriptResultKind::Success;
        return status;
    }

    static ScriptStatus exception() noexcept {
        ScriptStatus status;
        status.kind = ScriptResultKind::Exception;
        return status;
    }

    static ScriptStatus abort(ScriptAbortReason reason, std::int64_t position = -1) noexcept {
        ScriptStatus status;
        status.kind = ScriptResultKind::Abort;
        status.abort_payload = ScriptAbort{reason, position};
        return status;
    }

    [[nodiscard]] bool is_success() const noexcept { return kind == ScriptResultKind::Success; }

    [[nodiscard]] bool is_exception() const noexcept { return kind == ScriptResultKind::Exception; }

    [[nodiscard]] bool is_abort() const noexcept { return kind == ScriptResultKind::Abort; }

    [[nodiscard]] bool is_pending() const noexcept {
        return kind == ScriptResultKind::Abort && abort_payload.reason == ScriptAbortReason::None;
    }

    [[nodiscard]] bool is_done() const noexcept { return !is_pending(); }

    [[nodiscard]] const ScriptAbort &abort() const noexcept {
        FIBER_ASSERT(is_abort());
        return abort_payload;
    }

    [[nodiscard]] bool has_value() const noexcept { return is_success(); }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    ScriptResultKind kind = ScriptResultKind::Abort;
    ScriptAbort abort_payload{};
};

union ResultPayload {
    constexpr ResultPayload() : value{} {}

    fiber::json::JsValue value;
    fiber::json::JsValue exception;
    ScriptAbort abort;
};

struct alignas(16) ScriptResult {
    union Payload {
        constexpr Payload() : abort{} {}

        fiber::json::JsValue value;
        ScriptAbort abort;
    };

    constexpr ScriptResult() noexcept = default;

    ScriptResult(const fiber::json::JsValue &value) noexcept : kind(ScriptResultKind::Success), payload{} {
        payload.value = value;
    }

    ScriptResult(std::unexpected<fiber::json::JsValue> unexpected) noexcept :
        kind(ScriptResultKind::Exception), payload{} {
        payload.value = unexpected.error();
    }

    static ScriptResult success(const fiber::json::JsValue &value) noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Success;
        result.payload.value = value;
        return result;
    }

    static ScriptResult exception(const fiber::json::JsValue &value) noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Exception;
        result.payload.value = value;
        return result;
    }

    static ScriptResult abort(ScriptAbortReason reason, std::int64_t position = -1) noexcept {
        ScriptResult result;
        result.kind = ScriptResultKind::Abort;
        result.payload.abort = ScriptAbort{reason, position};
        return result;
    }

    [[nodiscard]] bool is_success() const noexcept { return kind == ScriptResultKind::Success; }

    [[nodiscard]] bool is_exception() const noexcept { return kind == ScriptResultKind::Exception; }

    [[nodiscard]] bool is_abort() const noexcept { return kind == ScriptResultKind::Abort; }

    [[nodiscard]] bool is_pending() const noexcept {
        return kind == ScriptResultKind::Abort && payload.abort.reason == ScriptAbortReason::None;
    }

    [[nodiscard]] bool is_done() const noexcept { return !is_pending(); }

    [[nodiscard]] const fiber::json::JsValue &value() const noexcept {
        FIBER_ASSERT(is_success());
        return payload.value;
    }

    [[nodiscard]] const fiber::json::JsValue &exception() const noexcept {
        FIBER_ASSERT(is_exception());
        return payload.value;
    }

    [[nodiscard]] const ScriptAbort &abort() const noexcept {
        FIBER_ASSERT(is_abort());
        return payload.abort;
    }

    [[nodiscard]] bool has_value() const noexcept { return is_success(); }

    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const fiber::json::JsValue &error() const noexcept { return exception(); }

    ScriptResultKind kind = ScriptResultKind::Abort;
    Payload payload{};
};

static_assert(std::is_trivially_copyable_v<ScriptAbort>);
static_assert(std::is_trivially_copyable_v<ScriptStatus>);
static_assert(std::is_trivially_copyable_v<ResultPayload>);
static_assert(std::is_trivially_copyable_v<ScriptResult>);

// ResultPayload mutators — the single way op functions write their outcome. Ops never carry a
// position (the dispatch layer recovers it from the current pc_), so set_abort takes only a reason.
inline CallResult set_value(ResultPayload &result, const fiber::json::JsValue &value) noexcept {
    result.value = value;
    return CallResult::Success;
}

inline CallResult set_undefined(ResultPayload &result) noexcept {
    result.value = fiber::json::JsValue::make_undefined();
    return CallResult::Success;
}

inline CallResult set_exception(ResultPayload &result, fiber::json::ExceptionKind kind) noexcept {
    result.exception = fiber::json::JsValue::make_exception(kind);
    return CallResult::Exception;
}

inline CallResult set_abort(ResultPayload &result, ScriptAbortReason reason) noexcept {
    result.abort = ScriptAbort{reason, -1};
    return CallResult::Abort;
}

} // namespace fiber::script

#endif // FIBER_SCRIPT_SCRIPT_RESULT_H
