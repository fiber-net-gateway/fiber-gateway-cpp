#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../common/json/JsGc.h"
#include "../async/AsyncExecutionContext.h"
#include "../ir/Compiled.h"
#include "VmError.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class InterpreterVm final : public AsyncExecutionContext, public fiber::json::GcRootSet::RootProvider {
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
                  async::IScheduler *scheduler,
                  ScriptRuntime &runtime);
    ~InterpreterVm() override;

    VmState iterate(VmResult &out);
    void set_resume_callback(ResumeCallback callback, void *context);

    ScriptRuntime &runtime() override;
    const fiber::json::JsValue &root() const override;
    void *attach() const override;
    const fiber::json::JsValue &arg_value(std::size_t index) const override;
    std::size_t arg_count() const override;
    async::IScheduler *scheduler() const override;
    void return_value(const fiber::json::JsValue &value) override;
    void throw_value(const fiber::json::JsValue &value) override;
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
    async::IScheduler *scheduler_ = nullptr;
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
    fiber::json::JsValue *arg_ptr_ = nullptr;
    std::size_t arg_cnt_ = 0;
    std::size_t arg_spread_slot_ = 0;
    VmError pending_error_{};
    bool has_error_ = false;
    enum class PendingValueKind {
        None,
        Thrown,
        AsyncReturn,
        AsyncThrow,
        Return
    };
    PendingValueKind pending_value_kind_ = PendingValueKind::None;
    fiber::json::JsValue pending_value_ = fiber::json::JsValue::make_undefined();
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

    void finalize_error(const VmError &error, VmResult &out);
    void notify_resume();
    void set_args_for_ctx(std::size_t off, std::size_t count);
    void set_args_for_spread(std::size_t slot);
    void clear_args();

    bool catch_for_exception(std::size_t epc);
    int search_catch(std::size_t epc) const;
    void build_exception_index();
    bool handle_error(VmError error, std::size_t epc);
    VmResult load_const(std::size_t operand_index);
    VmResult make_exception_value(const VmError &error);
    bool maybe_collect();
    bool apply_async_ready(VmResult &out);
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
