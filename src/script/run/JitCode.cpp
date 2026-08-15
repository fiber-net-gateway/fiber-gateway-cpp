#include <fiber/script/run/JitCode.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace fiber::script::run {

void NativeStackMapTable::set_records(std::vector<NativeStackMapRecord> records) noexcept {
    for (NativeStackMapRecord &record: records) {
        std::ranges::sort(record.stack_offsets);
        auto unique = std::ranges::unique(record.stack_offsets);
        record.stack_offsets.erase(unique.begin(), unique.end());
    }
    std::ranges::sort(records, {}, &NativeStackMapRecord::return_pc);
    records_ = std::move(records);
}

bool NativeStackMapTable::visit(const void *return_pc, const void *stack_pointer,
                                GcRootVisitor &visitor) const noexcept {
    if (!return_pc || !stack_pointer) {
        return false;
    }
    std::uintptr_t pc = reinterpret_cast<std::uintptr_t>(return_pc);
    auto it = std::ranges::lower_bound(records_, pc, {}, &NativeStackMapRecord::return_pc);
    if (it == records_.end() || it->return_pc != pc) {
        return false;
    }
    const auto *stack = static_cast<const std::byte *>(stack_pointer);
    for (std::int32_t offset: it->stack_offsets) {
        GcHeader *root = nullptr;
        std::memcpy(&root, stack + offset, sizeof(root));
        visitor.visit_heap_ref(root);
    }
    return true;
}

JitCode::JitCode(JitEntry entry, std::shared_ptr<const ir::Compiled> compiled, std::uint32_t async_value_count,
                 std::shared_ptr<const NativeStackMapTable> stack_maps, std::shared_ptr<void> resources) noexcept :
    entry_(entry), compiled_(std::move(compiled)), async_value_count_(async_value_count),
    stack_maps_(std::move(stack_maps)), resources_(std::move(resources)) {}

} // namespace fiber::script::run
