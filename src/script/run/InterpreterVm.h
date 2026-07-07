#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../AsyncTask.h"
#include "../ScriptResult.h"
#include "../ir/Compiled.h"
#include "../json/JsGc.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class InterpreterVm final : public fiber::json::GcRootSource {
public:
    InterpreterVm(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime);
    ~InterpreterVm() override;

    void iterate();
    [[nodiscard]] ScriptResult result() const noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] AsyncTask &async_task() noexcept { return async_.task; }
    void visit_roots(fiber::json::GcRootVisitor &visitor) noexcept override;

private:
    static constexpr int kInstrumentLen = 8;
    static constexpr int kIteratorLen = 12;
    static constexpr int kIteratorOff = kInstrumentLen + kIteratorLen;
    static constexpr int kMaxIteratorVar = (1 << kIteratorLen) - 1;

    enum class State : std::uint8_t {
        Init,
        Running,
        Suspend,
        Success,
        Exception,
        Abort,
    };

    enum class AsyncResumeKind { None, PushResult, ReplaceTop };

    struct AsyncState {
        AsyncTask task;
        ScriptStatus status = ScriptStatus::abort(ScriptAbortReason::InvalidState);
        fiber::json::JsValue value = fiber::json::JsValue::make_undefined();
        bool ready = false;
        AsyncResumeKind resume_kind = AsyncResumeKind::None;
        std::size_t resume_epc = 0;
        std::vector<fiber::json::JsValue> args;
        Library::Arguments arguments{};
    };

    const ir::Compiled &compile_;
    fiber::json::JsValue root_;
    void *attach_ = nullptr;
    ScriptRuntime &runtime_;

    std::unique_ptr<fiber::json::JsValue[]> slots_;
    fiber::json::JsValue *stack_ = nullptr;
    fiber::json::JsValue *vars_ = nullptr;
    std::size_t sp_ = 0;
    std::size_t pc_ = 0;
    State state_ = State::Init;
    ResultPayload result_{};
    AsyncState async_{};

    static void async_complete(void *context, ScriptStatus status) noexcept;
    fiber::json::JsValue *prepare_call_args(std::size_t off, std::size_t count);
    fiber::json::JsValue *prepare_spread_call_args(std::size_t slot, std::uint32_t &argc);
    Library::HostCallFrame make_call_frame() const;
    bool dispatch_func_const(std::uint8_t op, const ir::Compiled::FuncConst &func_const, std::uint32_t encoded_argc,
                             AsyncResumeKind resume_kind);
    bool apply_call_result(ScriptStatus status, const fiber::json::JsValue &value, AsyncResumeKind resume_kind,
                           std::size_t resume_epc);
    bool handle_call_result(CallResult status, std::size_t epc);

    bool catch_for_exception(std::size_t epc);
    bool handle_error(ScriptResult error, std::size_t epc);
    bool apply_async_ready();
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
