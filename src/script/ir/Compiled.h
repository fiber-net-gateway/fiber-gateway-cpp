#ifndef FIBER_SCRIPT_IR_COMPILED_H
#define FIBER_SCRIPT_IR_COMPILED_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../common/json/JsNode.h"
#include "../Library.h"
#include "Code.h"

namespace fiber::script::ir {

class Compiler;
class CompilerImpl;

class Compiled {
public:
    static constexpr std::uint32_t kNoPc = UINT32_MAX;

    struct FuncConst {
        void *user_data = nullptr;
        union {
            Library::Function sync_func = nullptr;
            Library::AsyncFunction async_func;
            Library::Constant sync_ct;
            Library::AsyncConstant async_ct;
        };
    };

    Compiled() noexcept = default;
    Compiled(const Compiled &) = delete;
    Compiled &operator=(const Compiled &) = delete;
    Compiled(Compiled &&other) noexcept;
    Compiled &operator=(Compiled &&other) noexcept;
    ~Compiled();

    [[nodiscard]] std::size_t stack_size() const noexcept { return stack_size_; }
    [[nodiscard]] std::size_t var_table_size() const noexcept { return var_table_size_; }
    [[nodiscard]] const std::int32_t *codes() const noexcept { return codes_; }
    [[nodiscard]] std::uint32_t code_size() const noexcept { return code_count_; }

    [[nodiscard]] const FuncConst &func_const(std::uint32_t index) const noexcept;
    [[nodiscard]] const fiber::json::JsValue &constant(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t find_catch(std::uint32_t epc) const noexcept;
    [[nodiscard]] std::uint32_t find_position(std::uint32_t pc) const noexcept;
    [[nodiscard]] bool contains_async() const noexcept;

private:
    friend class CompilerImpl;

    static constexpr std::uint32_t kNoPayload = UINT32_MAX;

    struct ConstantInit {
        fiber::json::JsValue value = fiber::json::JsValue::make_undefined();
        std::uint32_t payload_offset = kNoPayload;
    };

    static Compiled build(std::size_t stack_size, std::size_t var_table_size, std::span<const std::int32_t> codes,
                          std::span<const std::int32_t> positions, std::span<const ConstantInit> constants,
                          std::span<const FuncConst> func_consts, std::span<const std::uint32_t> exception_table,
                          std::span<const std::byte> payload);

    void reset() noexcept;
    void move_from(Compiled &other) noexcept;

    void *allocation_ = nullptr;
    std::size_t allocation_size_ = 0;
    std::size_t stack_size_ = 0;
    std::size_t var_table_size_ = 0;

    std::int32_t *codes_ = nullptr;
    std::int32_t *positions_ = nullptr;
    fiber::json::JsValue *constants_ = nullptr;
    FuncConst *func_consts_ = nullptr;
    std::uint32_t *catch_keys_ = nullptr;
    std::uint32_t *catch_targets_ = nullptr;
    std::byte *payload_ = nullptr;

    std::uint32_t code_count_ = 0;
    std::uint32_t constant_count_ = 0;
    std::uint32_t func_const_count_ = 0;
    std::uint32_t catch_count_ = 0;
    std::uint32_t payload_size_ = 0;
};

} // namespace fiber::script::ir

#endif // FIBER_SCRIPT_IR_COMPILED_H
