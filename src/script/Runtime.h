#ifndef FIBER_SCRIPT_RUNTIME_H
#define FIBER_SCRIPT_RUNTIME_H

#include <cstddef>
#include <vector>

#include "../common/mem/BufPool.h"
#include "JsGc.h"

namespace fiber::script {

class ConstValueHandle {
public:
    constexpr ConstValueHandle() noexcept = default;
    constexpr ConstValueHandle(std::nullptr_t) noexcept {}
    constexpr ConstValueHandle(fiber::script::JsValue *value) noexcept : value_(value) {}

    [[nodiscard]] const fiber::script::JsValue *get() const noexcept { return value_; }
    [[nodiscard]] const fiber::script::JsValue &operator*() const noexcept { return *value_; }
    [[nodiscard]] const fiber::script::JsValue *operator->() const noexcept { return value_; }
    [[nodiscard]] const fiber::script::JsValue &operator[](std::size_t index) const noexcept { return value_[index]; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    fiber::script::JsValue *value_ = nullptr;
};

class ValueHandle {
public:
    constexpr ValueHandle() noexcept = default;
    constexpr ValueHandle(std::nullptr_t) noexcept {}
    constexpr ValueHandle(fiber::script::JsValue *value) noexcept : value_(value) {}

    [[nodiscard]] fiber::script::JsValue *get() const noexcept { return value_; }
    [[nodiscard]] fiber::script::JsValue &operator*() const noexcept { return *value_; }
    [[nodiscard]] fiber::script::JsValue *operator->() const noexcept { return value_; }
    [[nodiscard]] fiber::script::JsValue &operator[](std::size_t index) const noexcept { return value_[index]; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value_ != nullptr; }
    [[nodiscard]] ConstValueHandle as_constant() const noexcept { return ConstValueHandle(value_); }
    [[nodiscard]] operator ConstValueHandle() const noexcept { return as_constant(); }

private:
    fiber::script::JsValue *value_ = nullptr;
};

static_assert(sizeof(ConstValueHandle) == sizeof(fiber::script::JsValue *));
static_assert(sizeof(ValueHandle) == sizeof(fiber::script::JsValue *));

constexpr bool operator==(ConstValueHandle handle, std::nullptr_t) noexcept { return !handle; }
constexpr bool operator==(std::nullptr_t, ConstValueHandle handle) noexcept { return !handle; }
constexpr bool operator!=(ConstValueHandle handle, std::nullptr_t) noexcept { return static_cast<bool>(handle); }
constexpr bool operator!=(std::nullptr_t, ConstValueHandle handle) noexcept { return static_cast<bool>(handle); }
constexpr bool operator==(ValueHandle handle, std::nullptr_t) noexcept { return !handle; }
constexpr bool operator==(std::nullptr_t, ValueHandle handle) noexcept { return !handle; }
constexpr bool operator!=(ValueHandle handle, std::nullptr_t) noexcept { return static_cast<bool>(handle); }
constexpr bool operator!=(std::nullptr_t, ValueHandle handle) noexcept { return static_cast<bool>(handle); }

class ScriptRuntime final : public fiber::script::GcRootSource {
public:
    class LocalMark;

    explicit ScriptRuntime(fiber::script::GcHeap &heap);
    ScriptRuntime(fiber::script::GcHeap &heap, fiber::mem::BufPool &pool);

    fiber::script::GcHeap &heap();
    const fiber::script::GcHeap &heap() const;

    [[nodiscard]] ValueHandle local_value();
    [[nodiscard]] ValueHandle global_value();

    void add_root_source(fiber::script::GcRootSource *source);
    void remove_root_source(fiber::script::GcRootSource *source);
    void visit_roots(fiber::script::GcRootVisitor &visitor) noexcept override;

    bool should_collect(std::size_t next_bytes = 0) const;
    void collect_now();
    void maybe_collect(std::size_t next_bytes = 0);

    template<typename AllocFn>
    auto alloc_with_gc(std::size_t next_bytes, AllocFn &&fn) -> decltype(fn()) {
        auto &&alloc_fn = fn;
        maybe_collect(next_bytes);
        auto result = alloc_fn();
        if (result) {
            return result;
        }
        collect_now();
        return alloc_fn();
    }

    template<typename OpFn>
    bool run_with_gc_retry(std::size_t next_bytes, OpFn &&fn) {
        auto &&op_fn = fn;
        maybe_collect(next_bytes);
        if (op_fn()) {
            return true;
        }
        collect_now();
        return op_fn();
    }

private:
    struct ValueBlock {
        ValueBlock *next = nullptr;
        fiber::script::JsValue slots[8];
    };

    struct LocalState {
        ValueBlock *block = nullptr;
        fiber::script::JsValue *top = nullptr;
    };

    friend class LocalMark;

    [[nodiscard]] LocalState mark_local() const noexcept;
    void restore_local(LocalState state) noexcept;
    [[nodiscard]] ValueBlock *alloc_value_block();
    [[nodiscard]] ValueBlock *acquire_local_block();
    [[nodiscard]] ValueBlock *acquire_global_block();
    static void reset_block(ValueBlock *block) noexcept;
    void recycle_local_blocks(ValueBlock *first) noexcept;

    fiber::script::GcHeap *heap_ = nullptr;
    fiber::mem::BufPool owned_pool_;
    fiber::mem::BufPool *pool_ = nullptr;

    ValueBlock *local_head_ = nullptr;
    ValueBlock *local_current_ = nullptr;
    ValueBlock *local_free_ = nullptr;
    fiber::script::JsValue *local_top_ = nullptr;
    fiber::script::JsValue *local_end_ = nullptr;

    ValueBlock *global_head_ = nullptr;
    ValueBlock *global_current_ = nullptr;
    fiber::script::JsValue *global_top_ = nullptr;
    fiber::script::JsValue *global_end_ = nullptr;

    std::vector<fiber::script::GcRootSource *> root_sources_;
};

class ScriptRuntime::LocalMark {
public:
    explicit LocalMark(ScriptRuntime &runtime) noexcept;
    LocalMark(const LocalMark &) = delete;
    LocalMark &operator=(const LocalMark &) = delete;
    LocalMark(LocalMark &&other) noexcept;
    LocalMark &operator=(LocalMark &&other) noexcept;
    ~LocalMark();

    void reset() noexcept;

private:
    ScriptRuntime *runtime_ = nullptr;
    LocalState state_{};
};

} // namespace fiber::script

#endif // FIBER_SCRIPT_RUNTIME_H
