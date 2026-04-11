#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../common/json/JsGc.h"
#include "../ir/Compiled.h"
#include "VmError.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class InterpreterVm final : public fiber::json::GcRootSet::RootProvider {
public:
    enum class VmState {
        Init,
        Running,
        Suspend,
        Success,
        Error
    };

    using ResumeCallback = void (*)(void *context);

    InterpreterVm(const ir::Compiled &compiled,
                  const fiber::json::JsValue &root,
                  void *attach,
                  ScriptRuntime &runtime);
    ~InterpreterVm() override;

    VmState iterate(VmResult &out);
    void set_resume_callback(ResumeCallback callback, void *context);
    void visit_roots(fiber::json::GcRootSet::RootVisitor &visitor) override;
    bool has_thrown() const;
    const fiber::json::JsValue &thrown() const;

private:
    static constexpr int kInstrumentLen = 8;
    static constexpr int kIteratorLen = 12;
    static constexpr int kIteratorOff = kInstrumentLen + kIteratorLen;
    static constexpr int kMaxIteratorVar = (1 << kIteratorLen) - 1;

    const ir::Compiled &compiled_;
    fiber::json::JsValue root_;
    void *attach_ = nullptr;
    ScriptRuntime &runtime_;

    std::vector<fiber::json::JsValue> slots_;
    fiber::json::JsValue *stack_ = nullptr;
    fiber::json::JsValue *vars_ = nullptr;
    std::size_t stack_size_ = 0;
    std::size_t var_count_ = 0;
    std::vector<fiber::json::JsValue> const_cache_;
    std::vector<bool> const_cache_valid_;
    std::vector<std::int32_t> exp_ins_;
    std::size_t sp_ = 0;
    std::size_t pc_ = 0;
    std::vector<fiber::json::JsValue> call_args_;
    VmError pending_error_{};
    bool has_error_ = false;
    enum class PendingValueKind {
        None,
        Thrown,
        Return
    };
    PendingValueKind pending_value_kind_ = PendingValueKind::None;
    fiber::json::JsValue pending_value_ = fiber::json::JsValue::make_undefined();
    Library::HostCallResult async_result_{};
    bool async_pending_ = false;
    bool async_ready_ = false;
    enum class AsyncResumeKind {
        None,
        PushResult,
        ReplaceTop
    };
    AsyncResumeKind async_resume_kind_ = AsyncResumeKind::None;
    std::size_t async_resume_epc_ = 0;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();
    VmState state_ = VmState::Init;
    bool in_iterate_ = false;
    bool resume_pending_ = false;
    ResumeCallback resume_callback_ = nullptr;
    void *resume_context_ = nullptr;

    static void async_complete(void *context, Library::HostCallResult result) noexcept;
    void finalize_error(const VmError &error, VmResult &out);
    void notify_resume();
    const fiber::json::JsValue *prepare_call_args(std::size_t off, std::size_t count);
    const fiber::json::JsValue *prepare_spread_call_args(std::size_t slot, std::uint32_t &argc);
    Library::HostCallFrame make_call_frame(const fiber::json::JsValue *args, std::uint32_t argc) const;
    VmError make_host_fault_error(const Library::HostFault &fault, std::size_t epc) const;
    bool dispatch_call_site(const ir::Compiled::CallSite &site,
                            AsyncResumeKind resume_kind,
                            VmResult &out);
    bool apply_call_result(const Library::HostCallResult &result,
                           AsyncResumeKind resume_kind,
                           std::size_t resume_epc,
                           VmResult &out,
                           bool from_async_completion);

    bool catch_for_exception(std::size_t epc);
    int search_catch(std::size_t epc) const;
    void build_exception_index();
    bool handle_error(VmError error, std::size_t epc);
    VmResult load_const(std::size_t operand_index);
    VmResult make_exception_value(const VmError &error);
    bool apply_async_ready(VmResult &out);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
