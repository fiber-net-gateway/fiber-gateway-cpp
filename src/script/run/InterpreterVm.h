#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include <fiber/script/AsyncTask.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/Library.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/ir/Compiled.h>

namespace fiber::script {
class GcHeap;
}

namespace fiber::script::run {

class InterpreterVm final : public fiber::script::GcRootSource {
public:
    InterpreterVm(const ir::Compiled &compiled, const fiber::script::JsValue &root, void *attach, GcHeap &runtime);
    ~InterpreterVm() override = default;

    void iterate();
    [[nodiscard]] ScriptResult result() const noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] AsyncTask &async_task() noexcept { return async_; }
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
        AsyncRetSuc,
        AsyncRetExp,
        AsyncRetAbort,
        Success,
        SuccessVoid,
        Exception,
        Abort,
    };

    const ir::Compiled &compile_;
    // Single self-contained frame: holds runtime (GcHeap&), root (JsValue), attach. Lives for the
    // whole exec_async coroutine, so a `const HostCallFrame&` passed to a lazy AsyncTask stays valid
    // across suspend/resume (no dangling stack local).
    Library::HostCallFrame frame_;
    fiber::script::GcRootRegistration reg_;

    std::unique_ptr<fiber::script::JsValue[]> slots_;
    fiber::script::JsValue *stack_ = nullptr;
    fiber::script::JsValue *vars_ = nullptr;
    std::size_t sp_ = 0;
    std::size_t pc_ = 0;
    State state_ = State::Init;
    ResultPayload result_{};
    AsyncTask async_{};

    static void async_complete(void *context, const AbiResult &result) noexcept;
    Library::Arguments make_call_arguments(std::uint8_t op, std::uint32_t encoded_argc, std::size_t &arg_base);
    bool dispatch_func_const(std::uint8_t op, const ir::Compiled::FuncConst &func_const, std::uint32_t encoded_argc);
    bool apply_call_result(const AbiResult &result, std::uint8_t op, std::uint32_t argc, std::size_t epc);
    bool handle_call_result(CallResult status, std::size_t epc);

    bool catch_for_exception(std::size_t epc);
    bool handle_error(AbiResult error, std::size_t epc);
    bool apply_async_result();
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
