#ifndef FIBER_SCRIPT_RUNTIME_H
#define FIBER_SCRIPT_RUNTIME_H

#include <cstddef>
#include <utility>
#include <vector>

#include "../common/json/JsGc.h"

namespace fiber::script {

class ScriptRuntime {
public:
    ScriptRuntime(fiber::json::GcHeap &heap, fiber::json::GcRootSet &roots);

    fiber::json::GcHeap &heap();
    const fiber::json::GcHeap &heap() const;
    fiber::json::GcRootSet &roots();
    const fiber::json::GcRootSet &roots() const;

    bool should_collect(std::size_t next_bytes = 0) const;
    void maybe_collect(std::size_t next_bytes = 0);

    template <typename AllocFn>
    auto alloc_with_gc(std::size_t next_bytes, AllocFn &&fn) -> decltype(fn()) {
        maybe_collect(next_bytes);
        return std::forward<AllocFn>(fn)();
    }

private:
    fiber::json::GcHeap *heap_ = nullptr;
    fiber::json::GcRootSet *roots_ = nullptr;
};

class GcRootGuard {
public:
    GcRootGuard(ScriptRuntime &runtime, fiber::json::JsValue *value);
    GcRootGuard(const GcRootGuard &) = delete;
    GcRootGuard &operator=(const GcRootGuard &) = delete;
    GcRootGuard(GcRootGuard &&) noexcept = default;
    GcRootGuard &operator=(GcRootGuard &&) noexcept = default;
    ~GcRootGuard() = default;

private:
    fiber::json::GcRootHandle handle_;
};

class TempRootScope {
public:
    explicit TempRootScope(ScriptRuntime &runtime);

    TempRootScope(const TempRootScope &) = delete;
    TempRootScope &operator=(const TempRootScope &) = delete;
    TempRootScope(TempRootScope &&) noexcept = default;
    TempRootScope &operator=(TempRootScope &&) noexcept = default;
    ~TempRootScope() = default;

    void add(fiber::json::JsValue *value);

private:
    fiber::json::GcRootSet *roots_ = nullptr;
    std::vector<fiber::json::GcRootHandle> handles_;
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_RUNTIME_H
