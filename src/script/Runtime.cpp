#include "Runtime.h"

namespace fiber::script {

ScriptRuntime::ScriptRuntime(fiber::json::GcHeap &heap, fiber::json::GcRootSet &roots)
    : heap_(&heap),
      roots_(&roots) {
}

fiber::json::GcHeap &ScriptRuntime::heap() {
    return *heap_;
}

const fiber::json::GcHeap &ScriptRuntime::heap() const {
    return *heap_;
}

fiber::json::GcRootSet &ScriptRuntime::roots() {
    return *roots_;
}

const fiber::json::GcRootSet &ScriptRuntime::roots() const {
    return *roots_;
}

bool ScriptRuntime::should_collect(std::size_t next_bytes) const {
    if (!heap_ || !roots_) {
        return false;
    }
    std::size_t used = fiber::json::gc_bytes_used(*heap_);
    std::size_t threshold = fiber::json::gc_threshold(*heap_);
    if (threshold == 0) {
        return false;
    }
    return used + next_bytes >= threshold;
}

void ScriptRuntime::maybe_collect(std::size_t next_bytes) {
    if (!should_collect(next_bytes)) {
        return;
    }
    fiber::json::gc_collect(*heap_, *roots_);
}

GcRootGuard::GcRootGuard(ScriptRuntime &runtime, fiber::json::JsValue *value)
    : handle_(runtime.roots(), value) {
}

TempRootScope::TempRootScope(ScriptRuntime &runtime)
    : roots_(&runtime.roots()) {
}

void TempRootScope::add(fiber::json::JsValue *value) {
    if (!roots_ || !value) {
        return;
    }
    handles_.emplace_back(*roots_, value);
}

} // namespace fiber::script
