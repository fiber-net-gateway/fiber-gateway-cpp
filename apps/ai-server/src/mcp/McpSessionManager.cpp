#include "McpSessionManager.h"

#include <charconv>

#include <common/Assert.h>

namespace fiber::ai_server {

McpStreamMailbox::McpStreamMailbox() {
    changed_publisher_ = changed_.acquire_publisher();
    FIBER_ASSERT(changed_publisher_.has_value());
}

bool McpStreamMailbox::push(std::string message) noexcept {
    bool accepted = false;
    {
        std::lock_guard lock(mutex_);
        if (!closed_ && messages_.size() < kMaxMessages) {
            messages_.push_back(std::move(message));
            accepted = true;
        } else {
            closed_ = true;
            messages_.clear();
        }
    }
    changed_publisher_->publish(accepted);
    return accepted;
}

std::vector<std::string> McpStreamMailbox::take() noexcept {
    std::lock_guard lock(mutex_);
    std::vector<std::string> output;
    output.reserve(messages_.size());
    while (!messages_.empty()) {
        output.push_back(std::move(messages_.front()));
        messages_.pop_front();
    }
    return output;
}

void McpStreamMailbox::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        messages_.clear();
    }
    changed_publisher_->publish(false);
}

bool McpStreamMailbox::closed() const noexcept {
    std::lock_guard lock(mutex_);
    return closed_;
}

McpSession::McpSession(std::string id, McpTransport transport, std::shared_ptr<const McpProjectRuntime> project,
                       std::chrono::steady_clock::time_point now) noexcept :
    id_(std::move(id)), transport_(transport), project_(std::move(project)), last_activity_(now) {}

std::shared_ptr<const McpProjectRuntime> McpSession::project() const noexcept {
    std::lock_guard lock(mutex_);
    return project_;
}

bool McpSession::begin_initialize(std::string protocol_version) noexcept {
    std::lock_guard lock(mutex_);
    if (state_ != McpSessionState::Created) {
        return false;
    }
    protocol_version_ = std::move(protocol_version);
    state_ = McpSessionState::Negotiated;
    return true;
}

bool McpSession::validate_initialized() const noexcept {
    std::lock_guard lock(mutex_);
    return state_ == McpSessionState::Negotiated || state_ == McpSessionState::Initialized;
}

void McpSession::mark_initialized() noexcept {
    std::lock_guard lock(mutex_);
    if (state_ == McpSessionState::Negotiated) {
        state_ = McpSessionState::Initialized;
    }
}

void McpSession::touch(std::chrono::steady_clock::time_point now) noexcept {
    std::lock_guard lock(mutex_);
    last_activity_ = now;
}

std::chrono::steady_clock::time_point McpSession::last_activity() const noexcept {
    std::lock_guard lock(mutex_);
    return last_activity_;
}

std::string McpSession::protocol_version() const {
    std::lock_guard lock(mutex_);
    return protocol_version_;
}

bool McpSession::attach_stream(const std::shared_ptr<McpStreamMailbox> &stream) noexcept {
    std::lock_guard lock(mutex_);
    if (state_ == McpSessionState::Closed || stream_) {
        return false;
    }
    stream_ = stream;
    return true;
}

void McpSession::detach_stream(const std::shared_ptr<McpStreamMailbox> &stream,
                               std::chrono::steady_clock::time_point now) noexcept {
    std::lock_guard lock(mutex_);
    if (stream_ == stream) {
        stream_.reset();
        last_activity_ = now;
    }
}

bool McpSession::emit(std::string message) noexcept {
    std::shared_ptr<McpStreamMailbox> stream;
    {
        std::lock_guard lock(mutex_);
        stream = stream_;
    }
    return stream && stream->push(std::move(message));
}

void McpSession::update_project(std::shared_ptr<const McpProjectRuntime> project) noexcept {
    std::lock_guard lock(mutex_);
    project_ = std::move(project);
}

bool McpSession::has_stream() const noexcept {
    std::lock_guard lock(mutex_);
    return stream_ != nullptr;
}

void McpSession::close() noexcept {
    std::lock_guard lock(mutex_);
    state_ = McpSessionState::Closed;
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
}

McpSessionState McpSession::state() const noexcept {
    std::lock_guard lock(mutex_);
    return state_;
}

McpSessionManager::McpSessionManager(std::string node_prefix) : node_prefix_(std::move(node_prefix)) {}

std::shared_ptr<McpSession> McpSessionManager::create(McpTransport transport,
                                                      std::shared_ptr<const McpProjectRuntime> project,
                                                      std::chrono::steady_clock::time_point now) {
    const std::uint64_t sequence = next_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    char suffix[16]{};
    const auto converted = std::to_chars(suffix, suffix + sizeof(suffix), sequence, 16);
    std::string id;
    id.reserve(node_prefix_.size() + static_cast<std::size_t>(converted.ptr - suffix));
    id.append(node_prefix_);
    id.append(suffix, converted.ptr);
    auto session = std::make_shared<McpSession>(id, transport, std::move(project), now);
    std::unique_lock lock(mutex_);
    sessions_.emplace(std::move(id), session);
    return session;
}

std::shared_ptr<McpSession> McpSessionManager::find(std::string_view session_id) const {
    std::shared_lock lock(mutex_);
    const auto it = sessions_.find(std::string(session_id));
    return it == sessions_.end() ? nullptr : it->second;
}

bool McpSessionManager::erase(std::string_view session_id) noexcept {
    std::shared_ptr<McpSession> removed;
    {
        std::unique_lock lock(mutex_);
        const auto it = sessions_.find(std::string(session_id));
        if (it == sessions_.end()) {
            return false;
        }
        removed = std::move(it->second);
        sessions_.erase(it);
    }
    removed->close();
    return true;
}

std::string_view McpSessionManager::parse_prefix(std::string_view session_id) const noexcept {
    if (node_prefix_.size() != 12 || session_id.size() <= node_prefix_.size()) {
        return {};
    }
    for (std::size_t i = 0; i < node_prefix_.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(session_id[i]);
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) {
            return {};
        }
    }
    return session_id.substr(0, node_prefix_.size());
}

bool McpSessionManager::is_local(std::string_view session_id) const noexcept {
    const std::string_view prefix = parse_prefix(session_id);
    return !prefix.empty() && prefix == node_prefix_;
}

std::size_t McpSessionManager::sweep(std::chrono::steady_clock::time_point now,
                                     std::chrono::seconds idle_timeout) noexcept {
    std::vector<std::shared_ptr<McpSession>> removed;
    {
        std::unique_lock lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (!it->second->has_stream() && it->second->last_activity() + idle_timeout <= now) {
                removed.push_back(std::move(it->second));
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto &session: removed) {
        session->close();
    }
    return removed.size();
}

void McpSessionManager::close_all() noexcept {
    std::unordered_map<std::string, std::shared_ptr<McpSession>> removed;
    {
        std::unique_lock lock(mutex_);
        removed.swap(sessions_);
    }
    for (const auto &[id, session]: removed) {
        (void) id;
        session->close();
    }
}

void McpSessionManager::update_project(std::string_view project_name, std::shared_ptr<const McpProjectRuntime> project,
                                       std::string message) noexcept {
    std::vector<std::shared_ptr<McpSession>> targets;
    {
        std::shared_lock lock(mutex_);
        targets.reserve(sessions_.size());
        for (const auto &[id, session]: sessions_) {
            (void) id;
            if (session->project()->name == project_name) {
                targets.push_back(session);
            }
        }
    }
    for (const auto &session: targets) {
        session->update_project(project);
        (void) session->emit(message);
    }
}

std::size_t McpSessionManager::size() const noexcept {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}

} // namespace fiber::ai_server
