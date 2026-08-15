#ifndef FIBER_SCRIPT_RUN_JIT_CODE_H
#define FIBER_SCRIPT_RUN_JIT_CODE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <fiber/script/GcRootSet.h>
#include <fiber/script/ir/Compiled.h>
#include <fiber/script/run/JitRuntime.h>

namespace fiber::script::run {

struct NativeStackMapRecord {
    std::uintptr_t return_pc = 0;
    std::vector<std::int32_t> stack_offsets;
};

class NativeStackMapTable final {
public:
    NativeStackMapTable() = default;
    NativeStackMapTable(const NativeStackMapTable &) = delete;
    NativeStackMapTable &operator=(const NativeStackMapTable &) = delete;

    void set_records(std::vector<NativeStackMapRecord> records) noexcept;
    [[nodiscard]] bool visit(const void *return_pc, const void *stack_pointer, GcRootVisitor &visitor) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

private:
    std::vector<NativeStackMapRecord> records_;
};

class JitCode final {
public:
    JitCode(JitEntry entry, std::shared_ptr<const ir::Compiled> compiled, std::uint32_t async_value_count,
            std::uint32_t inlined_operator_helper_count, std::shared_ptr<const NativeStackMapTable> stack_maps,
            std::shared_ptr<void> resources) noexcept;

    [[nodiscard]] JitEntry entry() const noexcept { return entry_; }
    [[nodiscard]] const std::shared_ptr<const ir::Compiled> &compiled() const noexcept { return compiled_; }
    [[nodiscard]] std::uint32_t async_value_count() const noexcept { return async_value_count_; }
    [[nodiscard]] std::uint32_t inlined_operator_helper_count() const noexcept {
        return inlined_operator_helper_count_;
    }
    [[nodiscard]] const NativeStackMapTable &stack_maps() const noexcept { return *stack_maps_; }

private:
    JitEntry entry_ = nullptr;
    std::shared_ptr<const ir::Compiled> compiled_;
    std::uint32_t async_value_count_ = 0;
    std::uint32_t inlined_operator_helper_count_ = 0;
    std::shared_ptr<const NativeStackMapTable> stack_maps_;
    std::shared_ptr<void> resources_;
};

} // namespace fiber::script::run

#endif // FIBER_SCRIPT_RUN_JIT_CODE_H
