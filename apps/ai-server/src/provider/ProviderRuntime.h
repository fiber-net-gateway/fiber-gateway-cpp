#ifndef FIBER_AI_SERVER_PROVIDER_RUNTIME_H
#define FIBER_AI_SERVER_PROVIDER_RUNTIME_H

#include "../config/LlmConfigSnapshot.h"

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <common/NonCopyable.h>
#include <common/NonMovable.h>

namespace fiber::ai_server {

class ProviderRuntimeState final : public common::NonCopyable, public common::NonMovable {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    [[nodiscard]] bool available(TimePoint now) noexcept;
    [[nodiscard]] bool token_available(std::string_view token_name, TimePoint now) noexcept;

    void record_success(std::string_view token_name) noexcept;
    void record_provider_failure(TimePoint now) noexcept;
    void mark_provider_unavailable(TimePoint now, std::chrono::milliseconds ttl) noexcept;
    void mark_token_unavailable(std::string_view token_name, TimePoint now, std::chrono::milliseconds ttl) noexcept;
    void retain_tokens(const std::vector<ProviderApiToken> &tokens);

    [[nodiscard]] std::size_t consecutive_failures() const noexcept { return consecutive_failures_; }
    [[nodiscard]] TimePoint provider_unavailable_until() const noexcept { return unavailable_until_; }

private:
    struct TokenState {
        std::string name;
        TimePoint unavailable_until{};
    };

    [[nodiscard]] TokenState *find_token(std::string_view name) noexcept;
    void clear_provider() noexcept;

    std::vector<TokenState> tokens_;
    TimePoint unavailable_until_{};
    std::size_t consecutive_failures_ = 0;
};

class ProviderRuntimeRegistry final : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] ProviderRuntimeState &state_for(std::string_view provider_name);
    void reconcile(const LlmProjectSnapshot &project);
    void clear() noexcept { states_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return states_.size(); }

private:
    // Nodes are intentionally retained until worker shutdown. Execution plans borrow
    // ProviderRuntimeState pointers across coroutine suspension, so config reconciliation
    // must not invalidate an in-flight request's state.
    std::map<std::string, std::unique_ptr<ProviderRuntimeState>, std::less<>> states_;
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_PROVIDER_RUNTIME_H
