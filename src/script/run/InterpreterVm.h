#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../common/json/JsGc.h"
#include "../AsyncTask.h"
#include "../ScriptResult.h"
#include "../ir/Compiled.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class InterpreterVm final : public fiber::json::GcRootSet::RootProvider {
public:
    InterpreterVm(const ir::Compiled &compiled, const fiber::json::JsValue &root, void *attach, ScriptRuntime &runtime);
    ~InterpreterVm() override;

    void iterate();
    [[nodiscard]] const ScriptResult &result() const noexcept { return result_; }
    [[nodiscard]] bool done() const noexcept { return result_.is_done(); }
    [[nodiscard]] AsyncTask &async_task() noexcept { return async_task_; }
    void visit_roots(fiber::json::GcRootSet::RootVisitor &visitor) override;

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
    enum class PendingValueKind { None, Thrown, Return };
    PendingValueKind pending_value_kind_ = PendingValueKind::None;
    fiber::json::JsValue pending_value_ = fiber::json::JsValue::make_undefined();
    ScriptResult result_;
    AsyncTask async_task_;
    ScriptResult async_result_;
    bool async_ready_ = false;
    enum class AsyncResumeKind { None, PushResult, ReplaceTop };
    AsyncResumeKind async_resume_kind_ = AsyncResumeKind::None;
    std::size_t async_resume_epc_ = 0;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();

    static void async_complete(void *context, const ScriptResult &result) noexcept;
    const fiber::json::JsValue *prepare_call_args(std::size_t off, std::size_t count);
    const fiber::json::JsValue *prepare_spread_call_args(std::size_t slot, std::uint32_t &argc);
    Library::HostCallFrame make_call_frame() const;
    bool dispatch_call_site(const ir::Compiled::CallSite &site, AsyncResumeKind resume_kind);
    bool apply_call_result(const ScriptResult &result, AsyncResumeKind resume_kind, std::size_t resume_epc);

    bool catch_for_exception(std::size_t epc);
    int search_catch(std::size_t epc) const;
    void build_exception_index();
    bool handle_error(ScriptResult error, std::size_t epc);
    ScriptResult load_const(std::size_t operand_index);
    bool apply_async_ready();
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_INTERPRETER_VM_H
