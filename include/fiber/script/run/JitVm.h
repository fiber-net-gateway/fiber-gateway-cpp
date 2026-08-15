#ifndef FIBER_SCRIPT_RUN_JIT_VM_H
#define FIBER_SCRIPT_RUN_JIT_VM_H

#include <cstdint>
#include <memory>

#include <fiber/script/AsyncTask.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/Library.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/run/JitCode.h>
#include <fiber/script/run/JitRuntime.h>

namespace fiber::script::run {

class JitVm final : public GcRootSource {
public:
    JitVm(std::shared_ptr<const JitCode> code, JsValue root, void *attach, GcHeap &heap);
    ~JitVm() override = default;

    void iterate() noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] ScriptResult result() const noexcept;
    [[nodiscard]] AsyncTask &async_task() noexcept { return async_; }
    void visit_roots(GcRootVisitor &visitor) noexcept override;

    [[nodiscard]] Library::HostCallFrame &host_frame() noexcept { return host_frame_; }
    [[nodiscard]] AsyncTask &runtime_async_task() noexcept { return async_; }
    [[nodiscard]] bool prepare_async_arguments(const JsValue *values, std::uint32_t count) noexcept;
    void arm_async_task() noexcept;

private:
    static void async_complete(void *context, const AbiResult &result) noexcept;
    void fail_allocation() noexcept;

    std::shared_ptr<const JitCode> code_;
    std::shared_ptr<const ir::Compiled> compiled_;
    Library::HostCallFrame host_frame_;
    GcRootRegistration registration_;
    JitFrameHeader frame_{};
    std::unique_ptr<JsValue[]> async_values_;
    std::unique_ptr<JsValue[]> async_arguments_;
    std::uint32_t async_argument_capacity_ = 0;
    AsyncTask async_{};
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_JIT_VM_H
