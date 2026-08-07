#ifndef FIBER_ACCESS_SERVER_ACCESS_TRACE_STATE_H
#define FIBER_ACCESS_SERVER_ACCESS_TRACE_STATE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>

#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>

namespace fiber::mem {
class BufPool;
}

namespace fiber::access_server {

// Request-local representation of the Java access-server tracestate contract.
// Non-bnrc members borrow the inbound request header. Decoded context and route
// updates live in the request pool.
class AccessTraceState final : public common::NonCopyable, public common::NonMovable {
public:
    explicit AccessTraceState(mem::BufPool &pool) noexcept;

    void parse(std::string_view trace_state) noexcept;

    [[nodiscard]] std::string_view inbound() const noexcept { return inbound_; }
    [[nodiscard]] bool should_override_upstream() const noexcept;
    [[nodiscard]] std::optional<std::string_view> get_context(std::string_view key) const noexcept;
    [[nodiscard]] common::IoResult<void> put_context(std::string_view key, std::string_view value) noexcept;
    [[nodiscard]] bool remove_context(std::string_view key) noexcept;
    [[nodiscard]] common::IoResult<std::string_view> encode() noexcept;

    template<typename Visitor>
    void for_each_context(Visitor &&visitor) const noexcept {
        using VisitorType = std::remove_reference_t<Visitor>;
        static_assert(std::is_nothrow_invocable_r_v<bool, VisitorType &, std::string_view, std::string_view>,
                      "AccessTraceState context visitor must be noexcept and return bool");

        void *opaque = const_cast<void *>(static_cast<const void *>(std::addressof(visitor)));
        for_each_context_impl(opaque, [](void *value, std::string_view key, std::string_view context_value) noexcept {
            return (*static_cast<VisitorType *>(value))(key, context_value);
        });
    }

private:
    struct StateMember;
    struct ContextEntry;
    using ContextVisitorFn = bool (*)(void *, std::string_view, std::string_view) noexcept;

    [[nodiscard]] bool append_state_member(std::string_view key, std::string_view value) noexcept;
    [[nodiscard]] bool put_context_view(std::string_view key, std::string_view value) noexcept;
    void invalidate_parse() noexcept;
    void for_each_context_impl(void *opaque, ContextVisitorFn visitor) const noexcept;

    mem::BufPool &pool_;
    std::string_view inbound_;
    std::string_view encoded_;
    StateMember *state_head_ = nullptr;
    StateMember *state_tail_ = nullptr;
    ContextEntry *context_head_ = nullptr;
    ContextEntry *context_tail_ = nullptr;
    std::size_t context_count_ = 0;
    bool parsed_member_ = false;
    bool rebuildable_ = true;
    bool dirty_ = true;
};

} // namespace fiber::access_server

#endif // FIBER_ACCESS_SERVER_ACCESS_TRACE_STATE_H
