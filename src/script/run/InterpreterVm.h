#ifndef FIBER_SCRIPT_RUN_INTERPRETER_VM_H
#define FIBER_SCRIPT_RUN_INTERPRETER_VM_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../common/json/JsGc.h"
#include "../ExecutionContext.h"
#include "../async/AsyncExecutionContext.h"
#include "../ir/Compiled.h"
#include "VmError.h"

namespace fiber::script {
class ScriptRuntime;
}

namespace fiber::script::run {

class InterpreterVm final : public ExecutionContext, public fiber::json::GcRootSet::RootProvider {
public:
    enum class AsyncExecState {
        Done,
        Pending
    };

    InterpreterVm(const ir::Compiled &compiled,
                  const fiber::json::JsValue &root,
                  void *attach,
                  async::IScheduler *scheduler,
                  ScriptRuntime &runtime);
    ~InterpreterVm() override;

    AsyncExecState exec_async(AsyncExecutionContext &context, VmResult &out);
    VmResult exec_sync();

    const fiber::json::JsValue &root() const override;
    void *attach() const override;
    const fiber::json::JsValue &arg_value(std::size_t index) const override;
    std::size_t arg_count() const override;
    async::IScheduler *scheduler() const override;
    void visit_roots(fiber::json::GcRootSet::RootVisitor &visitor) override;
    bool has_thrown() const;
    const fiber::json::JsValue &thrown() const;
    void set_async_result(const fiber::json::JsValue &value, bool is_throw);

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

    std::vector<fiber::json::JsValue> stack_;
    std::vector<fiber::json::JsValue> vars_;
    std::vector<fiber::json::JsValue> const_cache_;
    std::vector<bool> const_cache_valid_;
    std::vector<std::int32_t> exp_ins_;
    std::size_t sp_ = 0;
    std::size_t pc_ = 0;
    std::size_t arg_off_ = 0;
    std::size_t arg_cnt_ = 0;
    bool use_spread_args_ = false;
    fiber::json::JsValue spread_args_ = fiber::json::JsValue::make_undefined();
    VmError pending_error_{};
    bool has_error_ = false;
    fiber::json::JsValue thrown_ = fiber::json::JsValue::make_undefined();
    bool has_thrown_ = false;
    bool async_started_ = false;
    bool async_pending_ = false;
    bool async_ready_ = false;
    bool async_is_throw_ = false;
    fiber::json::JsValue async_value_ = fiber::json::JsValue::make_undefined();
    struct AsyncResumePoint {
        enum class Kind {
            None,
            PushResult,
            ReplaceTop
        };
        Kind kind = Kind::None;
        std::size_t slot = 0;
        std::size_t sp_after = 0;
        std::size_t epc = 0;
        bool clear_args = false;
    };
    AsyncResumePoint async_resume_{};
    fiber::json::JsValue return_value_ = fiber::json::JsValue::make_undefined();
    bool has_return_ = false;
    fiber::json::JsValue undefined_ = fiber::json::JsValue::make_undefined();

    void reset_state();
    void set_args_for_ctx(std::size_t off, std::size_t count);
    void set_args_for_ctx(const fiber::json::JsValue &args);
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
