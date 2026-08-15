#include <fiber/script/run/JitVm.h>

#include <algorithm>
#include <new>
#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::script::run {

JitVm::JitVm(std::shared_ptr<const JitCode> code, JsValue root, void *attach, GcHeap &heap) :
    code_(std::move(code)), compiled_(code_ ? code_->compiled() : nullptr), host_frame_(heap, root, attach),
    registration_(heap.roots(), *this) {
    frame_.vm_context = this;
    frame_.compiled = compiled_.get();
    frame_.root = &host_frame_.root;
    if (!code_ || !compiled_ || !code_->entry()) {
        frame_.state = JitRunState::Abort;
        frame_.abort = ScriptAbort{ScriptAbortReason::InvalidState, -1};
        return;
    }
    frame_.async_value_count = code_->async_value_count();
    if (frame_.async_value_count != 0) {
        async_values_.reset(new (std::nothrow) JsValue[frame_.async_value_count]);
        if (!async_values_) {
            fail_allocation();
            return;
        }
        std::fill_n(async_values_.get(), frame_.async_value_count, JsValue::make_undefined());
        frame_.async_values = async_values_.get();
    }
}

void JitVm::fail_allocation() noexcept {
    frame_.state = JitRunState::Abort;
    frame_.abort = ScriptAbort{ScriptAbortReason::OutOfMemory, -1};
}

bool JitVm::prepare_async_arguments(const JsValue *values, std::uint32_t count) noexcept {
    if (count > async_argument_capacity_) {
        std::uint32_t capacity = std::max(
                count, async_argument_capacity_ > UINT32_MAX / 2u ? UINT32_MAX : async_argument_capacity_ * 2u);
        std::unique_ptr<JsValue[]> replacement(new (std::nothrow) JsValue[capacity]);
        if (!replacement) {
            return false;
        }
        async_arguments_ = std::move(replacement);
        async_argument_capacity_ = capacity;
        frame_.async_arguments = async_arguments_.get();
    }
    if (count != 0) {
        if (!values || !async_arguments_) {
            return false;
        }
        std::copy_n(values, count, async_arguments_.get());
    }
    frame_.async_argument_count = count;
    return true;
}

void JitVm::arm_async_task() noexcept { async_.set_completion({&JitVm::async_complete, this}); }

void JitVm::iterate() noexcept {
    if (done()) {
        return;
    }
    if (frame_.state == JitRunState::Suspend) {
        return;
    }
    if (frame_.state == JitRunState::AsyncValue || frame_.state == JitRunState::AsyncException ||
        frame_.state == JitRunState::AsyncAbort) {
        if (!async_.valid()) {
            frame_.state = JitRunState::Abort;
            frame_.abort = ScriptAbort{ScriptAbortReason::InvalidState, -1};
            return;
        }
        async_.reset();
        frame_.async_argument_count = 0;
    }
    std::uint32_t raw_status = code_->entry()(&frame_);
    if (raw_status > static_cast<std::uint32_t>(JitStatus::SuccessVoid)) {
        frame_.state = JitRunState::Abort;
        frame_.abort = ScriptAbort{ScriptAbortReason::Internal, -1};
        return;
    }
    JitStatus status = static_cast<JitStatus>(raw_status);
    switch (status) {
        case JitStatus::Success:
            frame_.state = JitRunState::Success;
            break;
        case JitStatus::SuccessVoid:
            frame_.state = JitRunState::SuccessVoid;
            break;
        case JitStatus::Exception:
            frame_.state = JitRunState::Exception;
            break;
        case JitStatus::Abort:
            frame_.state = JitRunState::Abort;
            break;
        case JitStatus::Suspend:
            if (!async_.valid() || frame_.resume_id == 0) {
                frame_.state = JitRunState::Abort;
                frame_.abort = ScriptAbort{ScriptAbortReason::InvalidState, -1};
            } else {
                frame_.state = JitRunState::Suspend;
            }
            break;
    }
}

bool JitVm::done() const noexcept {
    return frame_.state == JitRunState::Success || frame_.state == JitRunState::SuccessVoid ||
           frame_.state == JitRunState::Exception || frame_.state == JitRunState::Abort;
}

ScriptResult JitVm::result() const noexcept {
    switch (frame_.state) {
        case JitRunState::Success:
            return ScriptResult::value(frame_.pending_value);
        case JitRunState::SuccessVoid:
            return ScriptResult::void_();
        case JitRunState::Exception:
            return ScriptResult::exception(frame_.pending_exception);
        case JitRunState::Abort:
            return ScriptResult::abort(frame_.abort.reason, frame_.abort.position);
        case JitRunState::Init:
        case JitRunState::Running:
        case JitRunState::Suspend:
        case JitRunState::AsyncValue:
        case JitRunState::AsyncException:
        case JitRunState::AsyncAbort:
            return ScriptResult::abort(ScriptAbortReason::None);
    }
    return ScriptResult::abort(ScriptAbortReason::Internal);
}

void JitVm::async_complete(void *context, const AbiResult &result) noexcept {
    auto *vm = static_cast<JitVm *>(context);
    if (!vm || vm->done() || vm->frame_.state != JitRunState::Suspend || !vm->async_.valid()) {
        return;
    }
    if (result.is_success()) {
        vm->frame_.pending_value = result.value();
        vm->frame_.state = JitRunState::AsyncValue;
    } else if (result.is_exception()) {
        vm->frame_.pending_exception = result.exception();
        vm->frame_.state = JitRunState::AsyncException;
    } else {
        vm->frame_.abort = result.abort();
        vm->frame_.state = JitRunState::AsyncAbort;
    }
}

void JitVm::visit_roots(GcRootVisitor &visitor) noexcept {
    visitor.visit(&host_frame_.root);
    visitor.visit_range(frame_.async_values, frame_.async_value_count);
    visitor.visit_range(frame_.async_arguments, frame_.async_argument_count);
    visitor.visit(&frame_.pending_value);
    visitor.visit(&frame_.pending_exception);
    if (frame_.safepoint_return_pc) {
        bool found =
                code_ && code_->stack_maps().visit(frame_.safepoint_return_pc, frame_.safepoint_stack_pointer, visitor);
        FIBER_ASSERT(found);
    }
}

} // namespace fiber::script::run
