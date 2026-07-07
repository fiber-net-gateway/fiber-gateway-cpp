#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../AsyncTask.h"
#include "../JsGc.h"
#include "../ScriptResult.h"
#include "../ir/Compiled.h"

namespace fiber::script {
class GcHeap;
}

namespace fiber::script::run {

class InterpreterVm final : public fiber::script::GcRootSource {
public:
    InterpreterVm(const ir::Compiled &compiled, const fiber::script::JsValue &root, void *attach, GcHeap &runtime);
    ~InterpreterVm() override;

    void iterate();
    [[nodiscard]] ScriptResult result() const noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] AsyncTask &async_task() noexcept { return async_.task; }
    void visit_roots(fiber::script::GcRootVisitor &visitor) noexcept override;

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
        fiber::script::JsValue value = fiber::script::JsValue::make_undefined();
        bool ready = false;
        AsyncResumeKind resume_kind = AsyncResumeKind::None;
        std::size_t resume_epc = 0;
        std::vector<fiber::script::JsValue> args;
        Library::Arguments arguments{};
    };

    const ir::Compiled &compile_;
    fiber::script::JsValue root_;
    void *attach_ = nullptr;
    GcHeap &runtime_;

    std::unique_ptr<fiber::script::JsValue[]> slots_;
    fiber::script::JsValue *stack_ = nullptr;
    fiber::script::JsValue *vars_ = nullptr;
    std::size_t sp_ = 0;
    std::size_t pc_ = 0;
    State state_ = State::Init;
    ResultPayload result_{};
    AsyncState async_{};

    static void async_complete(void *context, ScriptStatus status) noexcept;
    fiber::script::JsValue *prepare_call_args(std::size_t off, std::size_t count);
    fiber::script::JsValue *prepare_spread_call_args(std::size_t slot, std::uint32_t &argc);
    Library::HostCallFrame make_call_frame() const;
    bool dispatch_func_const(std::uint8_t op, const ir::Compiled::FuncConst &func_const, std::uint32_t encoded_argc,
                             AsyncResumeKind resume_kind);
    bool apply_call_result(ScriptStatus status, const fiber::script::JsValue &value, AsyncResumeKind resume_kind,
                           std::size_t resume_epc);
    bool handle_call_result(CallResult status, std::size_t epc);

    bool catch_for_exception(std::size_t epc);
    bool handle_error(ScriptResult error, std::size_t epc);
    bool apply_async_ready();
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
