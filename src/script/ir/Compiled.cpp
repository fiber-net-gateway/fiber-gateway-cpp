#include "Compiled.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <type_traits>
#include <vector>

#include "../../common/Assert.h"

namespace fiber::script::ir {

namespace {

static_assert(std::is_trivially_copyable_v<Compiled::FuncConst>);

constexpr std::size_t max_align(std::size_t a, std::size_t b) noexcept { return a > b ? a : b; }

constexpr std::size_t kAllocationAlign =
        max_align(max_align(max_align(alignof(std::int32_t), alignof(std::uint32_t)),
                            max_align(alignof(fiber::script::JsValue), alignof(Compiled::FuncConst))),
                  alignof(std::max_align_t));

std::size_t align_up(std::size_t value, std::size_t align) noexcept {
    const std::size_t mask = align - 1;
    return (value + mask) & ~mask;
}

CompileError make_compile_error(CompileErrorReason reason, const char *message) noexcept {
    return CompileError{reason, -1, message};
}

bool checked_add(std::size_t lhs, std::size_t rhs, std::size_t &out) noexcept {
    if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
        return false;
    }
    out = lhs + rhs;
    return true;
}

bool checked_mul(std::size_t lhs, std::size_t rhs, std::size_t &out) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs) {
        return false;
    }
    out = lhs * rhs;
    return true;
}

bool checked_align_up(std::size_t value, std::size_t align, std::size_t &out) noexcept {
    const std::size_t mask = align - 1;
    if (value > std::numeric_limits<std::size_t>::max() - mask) {
        return false;
    }
    out = (value + mask) & ~mask;
    return true;
}

bool add_region_size(std::size_t &total, std::size_t align, std::size_t element_size, std::size_t count) noexcept {
    std::size_t aligned = 0;
    if (!checked_align_up(total, align, aligned)) {
        return false;
    }
    std::size_t bytes = 0;
    if (!checked_mul(element_size, count, bytes)) {
        return false;
    }
    if (!checked_add(aligned, bytes, total)) {
        return false;
    }
    return true;
}

template<typename T>
T *assign_region(std::byte *base, std::size_t &offset, std::uint32_t count) noexcept {
    if (count == 0) {
        return nullptr;
    }
    offset = align_up(offset, alignof(T));
    T *result = reinterpret_cast<T *>(base + offset);
    offset += sizeof(T) * static_cast<std::size_t>(count);
    return result;
}

template<typename T>
void copy_region(T *dst, std::span<const T> src) noexcept {
    if (!dst || src.empty()) {
        return;
    }
    std::memcpy(dst, src.data(), sizeof(T) * src.size());
}

CompileResult<std::uint32_t> checked_count(std::size_t value, const char *message) noexcept {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(make_compile_error(CompileErrorReason::ProgramTooLarge, message));
    }
    return static_cast<std::uint32_t>(value);
}

struct ExceptionRange {
    std::uint32_t try_begin = 0;
    std::uint32_t catch_begin = 0;
    std::uint32_t catch_end = 0;
};

std::vector<std::uint32_t> build_catch_table(std::span<const std::uint32_t> exception_table) {
    if (exception_table.empty()) {
        return {};
    }
    FIBER_ASSERT((exception_table.size() % 3) == 0);
    std::vector<ExceptionRange> ranges;
    ranges.reserve(exception_table.size() / 3);
    for (std::size_t i = 0; i + 2 < exception_table.size(); i += 3) {
        ExceptionRange range{
                .try_begin = exception_table[i],
                .catch_begin = exception_table[i + 1],
                .catch_end = exception_table[i + 2],
        };
        FIBER_ASSERT(range.try_begin < range.catch_begin);
        FIBER_ASSERT(range.catch_begin < range.catch_end);
        ranges.push_back(range);
    }
    std::sort(ranges.begin(), ranges.end(), [](const ExceptionRange &lhs, const ExceptionRange &rhs) {
        if (lhs.try_begin != rhs.try_begin) {
            return lhs.try_begin < rhs.try_begin;
        }
        return lhs.catch_begin > rhs.catch_begin;
    });

    std::map<std::uint32_t, std::uint32_t> lookup;
    std::set<std::uint32_t> catches;
    for (const ExceptionRange &range: ranges) {
        lookup[range.try_begin] = range.catch_begin;
        catches.insert(range.catch_begin);
        auto latter = catches.lower_bound(range.catch_end);
        lookup[range.catch_begin] = latter == catches.end() ? Compiled::kNoPc : *latter;
    }

    std::vector<std::uint32_t> table;
    table.reserve(lookup.size() * 2);
    for (const auto &entry: lookup) {
        table.push_back(entry.first);
    }
    for (const auto &entry: lookup) {
        table.push_back(entry.second);
    }
    return table;
}

} // namespace

Compiled::Compiled(Compiled &&other) noexcept { move_from(other); }

Compiled &Compiled::operator=(Compiled &&other) noexcept {
    if (this != &other) {
        reset();
        move_from(other);
    }
    return *this;
}

Compiled::~Compiled() { reset(); }

const Compiled::FuncConst &Compiled::func_const(std::uint32_t index) const noexcept {
    FIBER_ASSERT(index < func_const_count_);
    return func_consts_[index];
}

const fiber::script::JsValue &Compiled::constant(std::uint32_t index) const noexcept {
    FIBER_ASSERT(index < constant_count_);
    return constants_[index];
}

std::uint32_t Compiled::find_catch(std::uint32_t epc) const noexcept {
    if (!catch_keys_ || catch_count_ == 0 || epc < catch_keys_[0]) {
        return kNoPc;
    }
    const std::uint32_t *begin = catch_keys_;
    const std::uint32_t *end = catch_keys_ + catch_count_;
    const std::uint32_t *upper = nullptr;
    if (catch_count_ <= 8) {
        upper = end;
        for (const std::uint32_t *it = begin; it != end; ++it) {
            if (*it > epc) {
                upper = it;
                break;
            }
        }
    } else {
        upper = std::upper_bound(begin, end, epc);
    }
    if (upper == begin) {
        return kNoPc;
    }
    const std::size_t index = static_cast<std::size_t>((upper - begin) - 1);
    return catch_targets_[index];
}

std::uint32_t Compiled::find_position(std::uint32_t pc) const noexcept {
    if (!positions_ || pc >= code_count_) {
        return kNoPc;
    }
    const std::int32_t position = positions_[pc];
    if (position < 0) {
        return kNoPc;
    }
    return static_cast<std::uint32_t>(position);
}

bool Compiled::contains_async() const noexcept {
    for (std::uint32_t i = 0; i < code_count_; ++i) {
        switch (static_cast<std::uint32_t>(codes_[i]) & 0xFFu) {
            case Code::CALL_ASYNC_CONST:
            case Code::CALL_ASYNC_FUNC:
            case Code::CALL_ASYNC_FUNC_SPREAD:
                return true;
            default:
                break;
        }
    }
    return false;
}

CompileResult<Compiled> Compiled::build(std::size_t stack_size, std::size_t var_table_size,
                                        std::span<const std::int32_t> codes, std::span<const std::int32_t> positions,
                                        std::span<const ConstantInit> constants, std::span<const FuncConst> func_consts,
                                        std::span<const std::uint32_t> exception_table,
                                        std::span<const std::byte> payload) {
    if (codes.size() != positions.size()) {
        return std::unexpected(
                make_compile_error(CompileErrorReason::Internal, "compiled code position table mismatch"));
    }

    std::vector<std::uint32_t> catch_table = build_catch_table(exception_table);
    auto catch_count_result = checked_count(catch_table.size() / 2, "compiled catch table is too large");
    if (!catch_count_result) {
        return std::unexpected(catch_count_result.error());
    }
    const std::uint32_t catch_count = catch_count_result.value();

    std::size_t total = 0;
    bool sized =
            add_region_size(total, alignof(std::int32_t), sizeof(std::int32_t), codes.size()) &&
            add_region_size(total, alignof(std::int32_t), sizeof(std::int32_t), positions.size()) &&
            add_region_size(total, alignof(fiber::script::JsValue), sizeof(fiber::script::JsValue), constants.size()) &&
            add_region_size(total, alignof(FuncConst), sizeof(FuncConst), func_consts.size()) &&
            add_region_size(total, alignof(std::uint32_t), sizeof(std::uint32_t), catch_count) &&
            add_region_size(total, alignof(std::uint32_t), sizeof(std::uint32_t), catch_count) &&
            add_region_size(total, alignof(std::byte), sizeof(std::byte), payload.size());
    if (!sized) {
        return std::unexpected(
                make_compile_error(CompileErrorReason::ProgramTooLarge, "compiled program is too large"));
    }

    Compiled result;
    result.stack_size_ = stack_size;
    result.var_table_size_ = var_table_size;
    auto code_count = checked_count(codes.size(), "compiled code table is too large");
    auto constant_count = checked_count(constants.size(), "compiled constant table is too large");
    auto func_const_count = checked_count(func_consts.size(), "compiled function table is too large");
    auto payload_size = checked_count(payload.size(), "compiled payload is too large");
    if (!code_count) {
        return std::unexpected(code_count.error());
    }
    if (!constant_count) {
        return std::unexpected(constant_count.error());
    }
    if (!func_const_count) {
        return std::unexpected(func_const_count.error());
    }
    if (!payload_size) {
        return std::unexpected(payload_size.error());
    }
    result.code_count_ = code_count.value();
    result.constant_count_ = constant_count.value();
    result.func_const_count_ = func_const_count.value();
    result.catch_count_ = catch_count;
    result.payload_size_ = payload_size.value();
    if (total == 0) {
        return result;
    }

    result.allocation_ = ::operator new(total, std::align_val_t(kAllocationAlign), std::nothrow);
    if (!result.allocation_) {
        result.reset();
        return std::unexpected(make_compile_error(CompileErrorReason::OutOfMemory, "compiled allocation failed"));
    }
    result.allocation_size_ = total;

    auto *base = static_cast<std::byte *>(result.allocation_);
    std::size_t offset = 0;
    result.codes_ = assign_region<std::int32_t>(base, offset, result.code_count_);
    result.positions_ = assign_region<std::int32_t>(base, offset, result.code_count_);
    result.constants_ = assign_region<fiber::script::JsValue>(base, offset, result.constant_count_);
    result.func_consts_ = assign_region<FuncConst>(base, offset, result.func_const_count_);
    result.catch_keys_ = assign_region<std::uint32_t>(base, offset, result.catch_count_);
    result.catch_targets_ = assign_region<std::uint32_t>(base, offset, result.catch_count_);
    result.payload_ = assign_region<std::byte>(base, offset, result.payload_size_);

    copy_region(result.codes_, codes);
    copy_region(result.positions_, positions);
    copy_region(result.func_consts_, func_consts);
    if (result.catch_count_ > 0) {
        std::memcpy(result.catch_keys_, catch_table.data(), sizeof(std::uint32_t) * result.catch_count_);
        std::memcpy(result.catch_targets_, catch_table.data() + result.catch_count_,
                    sizeof(std::uint32_t) * result.catch_count_);
    }
    if (result.payload_size_ > 0) {
        std::memcpy(result.payload_, payload.data(), result.payload_size_);
    }
    for (std::uint32_t i = 0; i < result.constant_count_; ++i) {
        fiber::script::JsValue value = constants[i].value;
        if (constants[i].payload_offset != kNoPayload) {
            FIBER_ASSERT(constants[i].payload_offset <= result.payload_size_);
            const char *data = reinterpret_cast<const char *>(result.payload_ + constants[i].payload_offset);
            if (fiber::script::js_value_is_borrowed_string(value)) {
                value.payload = reinterpret_cast<std::uint64_t>(data);
            } else if (fiber::script::js_value_is_borrowed_binary(value)) {
                value.payload = reinterpret_cast<std::uint64_t>(data);
            }
        }
        result.constants_[i] = value;
    }

    return result;
}

void Compiled::reset() noexcept {
    if (allocation_) {
        ::operator delete(allocation_, std::align_val_t(kAllocationAlign));
    }
    allocation_ = nullptr;
    allocation_size_ = 0;
    stack_size_ = 0;
    var_table_size_ = 0;
    codes_ = nullptr;
    positions_ = nullptr;
    constants_ = nullptr;
    func_consts_ = nullptr;
    catch_keys_ = nullptr;
    catch_targets_ = nullptr;
    payload_ = nullptr;
    code_count_ = 0;
    constant_count_ = 0;
    func_const_count_ = 0;
    catch_count_ = 0;
    payload_size_ = 0;
}

void Compiled::move_from(Compiled &other) noexcept {
    allocation_ = other.allocation_;
    allocation_size_ = other.allocation_size_;
    stack_size_ = other.stack_size_;
    var_table_size_ = other.var_table_size_;
    codes_ = other.codes_;
    positions_ = other.positions_;
    constants_ = other.constants_;
    func_consts_ = other.func_consts_;
    catch_keys_ = other.catch_keys_;
    catch_targets_ = other.catch_targets_;
    payload_ = other.payload_;
    code_count_ = other.code_count_;
    constant_count_ = other.constant_count_;
    func_const_count_ = other.func_const_count_;
    catch_count_ = other.catch_count_;
    payload_size_ = other.payload_size_;

    other.allocation_ = nullptr;
    other.allocation_size_ = 0;
    other.stack_size_ = 0;
    other.var_table_size_ = 0;
    other.codes_ = nullptr;
    other.positions_ = nullptr;
    other.constants_ = nullptr;
    other.func_consts_ = nullptr;
    other.catch_keys_ = nullptr;
    other.catch_targets_ = nullptr;
    other.payload_ = nullptr;
    other.code_count_ = 0;
    other.constant_count_ = 0;
    other.func_const_count_ = 0;
    other.catch_count_ = 0;
    other.payload_size_ = 0;
}

} // namespace fiber::script::ir
