#ifndef FIBER_HTTP_HTTP2_CLOSE_GATE_H
#define FIBER_HTTP_HTTP2_CLOSE_GATE_H

#include <cstddef>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Connection.h"

namespace fiber::http {

// The owner forwards Closed to this gate. Completion is deferred until the
// connection stack unwinds. Observers run before joiners are posted; observers
// must not destroy the owner/gate or drive a nested event loop.
// Lives on one EventLoop and is not thread safe.
class Http2CloseGate : public common::NonCopyable, public common::NonMovable {
public:
    using CloseResult = Http2Connection::CloseResult;
    using ObserverCallback = void (*)(void *ctx, Http2Connection &connection, common::IoErr reason) noexcept;

    // Embedded in the observer, so subscribing allocates nothing. Unsubscribes
    // itself, so an observer may be destroyed before the connection closes.
    struct ObserverHook {
        ObserverHook() noexcept = default;
        ObserverHook(const ObserverHook &) = delete;
        ObserverHook &operator=(const ObserverHook &) = delete;
        ~ObserverHook();

        Http2CloseGate *gate = nullptr;
        ObserverCallback callback = nullptr;
        void *ctx = nullptr;
        ObserverHook *prev = nullptr;
        ObserverHook *next = nullptr;
        bool linked = false;
    };

    Http2CloseGate(event::EventLoop &loop, Http2Connection &connection) noexcept;
    // Resolves outstanding joiners with Canceled; their coroutines resume on the
    // loop after the gate is gone, so this is a teardown-only path.
    ~Http2CloseGate();

    void on_connection_closed() noexcept;

    [[nodiscard]] fiber::async::Task<CloseResult> join() noexcept;

    void add_observer(ObserverHook &hook, ObserverCallback callback, void *ctx) noexcept;
    void remove_observer(ObserverHook &hook) noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] common::IoErr terminal_error() const noexcept;
    [[nodiscard]] bool has_joiners() const noexcept { return joiner_head_ != nullptr; }
    [[nodiscard]] std::size_t joiner_count() const noexcept { return joiner_count_; }

private:
    class Joiner;

    enum class Phase : std::uint8_t { Open, Pending, Dispatching, Complete };
    static void on_completion(Http2CloseGate *gate) noexcept;
    void dispatch() noexcept;
    void link_joiner(Joiner &joiner) noexcept;
    void unlink_joiner(Joiner &joiner) noexcept;

    event::EventLoop *loop_;
    Http2Connection *connection_;
    event::EventLoop::DeferEntry completion_entry_{};
    Joiner *joiner_head_ = nullptr;
    Joiner *joiner_tail_ = nullptr;
    ObserverHook *observer_head_ = nullptr;
    ObserverHook *observer_tail_ = nullptr;
    std::size_t joiner_count_ = 0;
    common::IoErr terminal_error_ = common::IoErr::None;
    Phase phase_ = Phase::Open;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CLOSE_GATE_H
