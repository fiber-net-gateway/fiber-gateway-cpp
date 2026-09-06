#ifndef FIBER_HTTP_HTTP2_CLOSE_GATE_H
#define FIBER_HTTP_HTTP2_CLOSE_GATE_H

#include <cstddef>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Connection.h"

namespace fiber::http {

// Fans one connection's single close notification out to everyone who cares.
//
// Http2Connection reports closure through one callback that fires once. This
// gate takes that callback and turns it into any number of subscribers: plain
// observers, which run inline in registration order, and coroutines parked in
// join(), which resume afterwards on the loop. Observers therefore always see
// the closure before a joiner can act on it - a pool has unlinked its entry
// before anyone awaiting the connection gets a chance to destroy it.
//
// Lives on the connection's EventLoop and is not thread safe.
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

    Http2CloseGate() noexcept = default;
    // Resolves outstanding joiners with Canceled; their coroutines resume on the
    // loop after the gate is gone, so this is a teardown-only path.
    ~Http2CloseGate();

    // Installs the gate as the connection's close callback. Safe before or
    // after start(): a connection that already reported closure resolves
    // join() immediately.
    void arm(Http2Connection &connection) noexcept;

    [[nodiscard]] fiber::async::Task<CloseResult> join() noexcept;

    void add_observer(ObserverHook &hook, ObserverCallback callback, void *ctx) noexcept;
    void remove_observer(ObserverHook &hook) noexcept;

    [[nodiscard]] bool armed() const noexcept { return connection_ != nullptr; }
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] common::IoErr terminal_error() const noexcept;
    [[nodiscard]] bool has_joiners() const noexcept { return joiner_head_ != nullptr; }
    [[nodiscard]] std::size_t joiner_count() const noexcept { return joiner_count_; }

private:
    class Joiner;

    static void on_connection_closed(void *ctx, Http2Connection &connection, CloseResult result) noexcept;
    void dispatch(common::IoErr reason) noexcept;
    void link_joiner(Joiner &joiner) noexcept;
    void unlink_joiner(Joiner &joiner) noexcept;

    Http2Connection *connection_ = nullptr;
    Joiner *joiner_head_ = nullptr;
    Joiner *joiner_tail_ = nullptr;
    ObserverHook *observer_head_ = nullptr;
    ObserverHook *observer_tail_ = nullptr;
    std::size_t joiner_count_ = 0;
    common::IoErr terminal_error_ = common::IoErr::None;
    bool closed_ = false;
    bool installed_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP2_CLOSE_GATE_H
