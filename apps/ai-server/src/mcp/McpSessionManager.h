#ifndef FIBER_AI_SERVER_MCP_SESSION_MANAGER_H
#define FIBER_AI_SERVER_MCP_SESSION_MANAGER_H

#include "McpConfigSnapshot.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <async/Watch.h>
#include <common/NonCopyable.h>
#include <common/NonMovable.h>

namespace fiber::ai_server {

enum class McpTransport : std::uint8_t {
    StreamableHttp,
    LegacySse,
};

enum class McpSessionState : std::uint8_t {
    Created,
    Negotiated,
    Initialized,
    Closed,
};

class McpStreamMailbox final : public common::NonCopyable, public common::NonMovable {
public:
    static constexpr std::size_t kMaxMessages = 64;

    McpStreamMailbox();

    [[nodiscard]] bool push(std::string message) noexcept;
    [[nodiscard]] std::vector<std::string> take() noexcept;
    void close() noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] async::Watch<bool>::Subscriber subscribe() { return changed_.subscribe(); }

private:
    mutable std::mutex mutex_;
    std::deque<std::string> messages_;
    bool closed_ = false;
    async::Watch<bool> changed_{false};
    std::optional<async::Watch<bool>::Publisher> changed_publisher_;
};

class McpSession final : public common::NonCopyable, public common::NonMovable {
public:
    McpSession(std::string id, McpTransport transport, std::shared_ptr<const McpProjectRuntime> project,
               std::chrono::steady_clock::time_point now) noexcept;

    [[nodiscard]] std::string_view id() const noexcept { return id_; }
    [[nodiscard]] McpTransport transport() const noexcept { return transport_; }
    [[nodiscard]] std::shared_ptr<const McpProjectRuntime> project() const noexcept;

    [[nodiscard]] bool begin_initialize(std::string protocol_version) noexcept;
    [[nodiscard]] bool validate_initialized() const noexcept;
    void mark_initialized() noexcept;
    void touch(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point last_activity() const noexcept;
    [[nodiscard]] std::string protocol_version() const;
    [[nodiscard]] bool attach_stream(const std::shared_ptr<McpStreamMailbox> &stream) noexcept;
    void detach_stream(const std::shared_ptr<McpStreamMailbox> &stream,
                       std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool emit(std::string message) noexcept;
    void update_project(std::shared_ptr<const McpProjectRuntime> project) noexcept;
    [[nodiscard]] bool has_stream() const noexcept;
    void close() noexcept;
    [[nodiscard]] McpSessionState state() const noexcept;

private:
    std::string id_;
    McpTransport transport_ = McpTransport::StreamableHttp;
    std::shared_ptr<const McpProjectRuntime> project_;
    mutable std::mutex mutex_;
    std::string protocol_version_;
    std::chrono::steady_clock::time_point last_activity_{};
    McpSessionState state_ = McpSessionState::Created;
    std::shared_ptr<McpStreamMailbox> stream_;
};

class McpSessionManager final : public common::NonCopyable, public common::NonMovable {
public:
    explicit McpSessionManager(std::string node_prefix);

    [[nodiscard]] std::shared_ptr<McpSession>
    create(McpTransport transport, std::shared_ptr<const McpProjectRuntime> project,
           std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    [[nodiscard]] std::shared_ptr<McpSession> find(std::string_view session_id) const;
    [[nodiscard]] bool erase(std::string_view session_id) noexcept;
    [[nodiscard]] std::string_view local_prefix() const noexcept { return node_prefix_; }
    [[nodiscard]] std::string_view parse_prefix(std::string_view session_id) const noexcept;
    [[nodiscard]] bool is_local(std::string_view session_id) const noexcept;
    [[nodiscard]] std::size_t sweep(std::chrono::steady_clock::time_point now,
                                    std::chrono::seconds idle_timeout = std::chrono::seconds(60)) noexcept;
    void close_all() noexcept;
    void update_project(std::string_view project_name, std::shared_ptr<const McpProjectRuntime> project,
                        std::string message) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::string node_prefix_;
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<McpSession>> sessions_;
    std::atomic<std::uint64_t> next_id_{0};
};

} // namespace fiber::ai_server

#endif // FIBER_AI_SERVER_MCP_SESSION_MANAGER_H
