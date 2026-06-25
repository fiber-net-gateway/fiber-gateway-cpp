#include "QuicSendScheduler.h"

#include <expected>

#include "../async/Yield.h"
#include "../common/Assert.h"
#include "QuicUdpEndpoint.h"

namespace fiber::quic {

class QuicSendScheduler::WaitForWorkAwaiter {
public:
    explicit WaitForWorkAwaiter(QuicSendScheduler &scheduler) noexcept : scheduler_(&scheduler) {}
    WaitForWorkAwaiter(const WaitForWorkAwaiter &) = delete;
    WaitForWorkAwaiter &operator=(const WaitForWorkAwaiter &) = delete;
    WaitForWorkAwaiter(WaitForWorkAwaiter &&) = delete;
    WaitForWorkAwaiter &operator=(WaitForWorkAwaiter &&) = delete;

    ~WaitForWorkAwaiter() {
        if (scheduler_) {
            scheduler_->cancel_waiter(this);
        }
    }

    bool await_ready() const noexcept { return scheduler_ == nullptr || scheduler_->should_wake_waiter(); }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        FIBER_ASSERT(scheduler_ != nullptr);
        loop_ = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop_ != nullptr);
        handle_ = handle;
        return scheduler_->arm_waiter(this);
    }

    void await_resume() noexcept {
        if (!scheduler_) {
            return;
        }
        if (scheduler_->waiter_ == this) {
            scheduler_->waiter_ = nullptr;
        }
        scheduler_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        resume_posted_ = false;
    }

private:
    static void on_notify(WaitForWorkAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        awaiter->resume_posted_ = false;
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    QuicSendScheduler *scheduler_ = nullptr;
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::NotifyEntry notify_entry_{};
    bool resume_posted_ = false;

    friend class QuicSendScheduler;
};

QuicSendScheduler::QuicSendScheduler() noexcept = default;

QuicSendScheduler::~QuicSendScheduler() {
    close();
    FIBER_ASSERT(!running_);
}

common::IoResult<void> QuicSendScheduler::init(event::EventLoop &loop, net::UdpSocket &socket,
                                               QuicUdpEndpoint &endpoint, const Options &options) noexcept {
    if (initialized_ || options.max_packets_per_wakeup == 0 || options.max_packets_per_connection == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    loop_ = &loop;
    socket_ = &socket;
    endpoint_ = &endpoint;
    options_ = options;
    stop_reason_ = common::IoErr::None;
    closing_ = false;
    initialized_ = true;
    return {};
}

void QuicSendScheduler::submit(QuicConnection &connection) noexcept {
    if (!initialized_ || closing_) {
        return;
    }
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    enqueue_ready(connection);
    notify_waiter();
}

void QuicSendScheduler::remove(QuicConnection &connection) noexcept {
    auto &entry = connection.send_queue_entry;
    if (entry.link.linked()) {
        ready_.erase(entry);
    }
    entry.connection = nullptr;
}

void QuicSendScheduler::close(common::IoErr reason) noexcept {
    if (!initialized_) {
        return;
    }
    if (stop_reason_ == common::IoErr::None) {
        stop_reason_ = reason;
    }
    closing_ = true;
    clear_ready();
    notify_waiter();
}

async::Task<void> QuicSendScheduler::run() noexcept {
    auto *current = event::EventLoop::current_or_null();
    FIBER_ASSERT(current != nullptr);
    FIBER_ASSERT(loop_ == current);
    FIBER_ASSERT(!running_);

    running_ = true;
    while (!closing_) {
        if (!has_work()) {
            co_await WaitForWorkAwaiter(*this);
            continue;
        }

        std::size_t packets_this_wakeup = 0;
        while (!closing_ && has_work()) {
            QuicConnection *connection = front_ready();
            if (connection == nullptr) {
                break;
            }

            common::IoErr err = co_await flush_connection(*connection);
            if (err != common::IoErr::None && err != common::IoErr::WouldBlock) {
                connection->close(QuicErrorCode::InternalError);
                remove(*connection);
            }

            ++packets_this_wakeup;
            if (packets_this_wakeup >= options_.max_packets_per_wakeup) {
                co_await async::yield();
                packets_this_wakeup = 0;
            }
        }
    }

    running_ = false;
}

bool QuicSendScheduler::should_wake_waiter() const noexcept { return closing_ || has_work(); }

bool QuicSendScheduler::arm_waiter(WaitForWorkAwaiter *awaiter) noexcept {
    if (!awaiter || should_wake_waiter()) {
        return false;
    }
    FIBER_ASSERT(waiter_ == nullptr);
    waiter_ = awaiter;
    return true;
}

void QuicSendScheduler::cancel_waiter(WaitForWorkAwaiter *awaiter) noexcept {
    if (waiter_ == awaiter) {
        waiter_ = nullptr;
    }
}

void QuicSendScheduler::notify_waiter() noexcept {
    if (!waiter_ || waiter_->resume_posted_ || waiter_->loop_ == nullptr) {
        return;
    }
    waiter_->resume_posted_ = true;
    waiter_->loop_->post<WaitForWorkAwaiter, &WaitForWorkAwaiter::notify_entry_, &WaitForWorkAwaiter::on_notify>(
            *waiter_);
}

bool QuicSendScheduler::has_work() const noexcept { return !ready_.empty(); }

void QuicSendScheduler::enqueue_ready(QuicConnection &connection) noexcept {
    auto &entry = connection.send_queue_entry;
    if (entry.link.linked()) {
        return;
    }
    entry.connection = &connection;
    ready_.push_back(entry);
}

void QuicSendScheduler::rotate_front_to_back(QuicConnection &connection) noexcept {
    auto &entry = connection.send_queue_entry;
    if (!entry.link.linked() || ready_.front() != &entry || ready_.back() == &entry) {
        return;
    }
    ready_.erase(entry);
    ready_.push_back(entry);
}

QuicConnection *QuicSendScheduler::front_ready() noexcept {
    auto *entry = ready_.front();
    if (!entry) {
        return nullptr;
    }
    if (entry->connection == nullptr) {
        ready_.erase(*entry);
        return nullptr;
    }
    return entry->connection;
}

void QuicSendScheduler::clear_ready() noexcept {
    while (!ready_.empty()) {
        auto *entry = ready_.front();
        ready_.erase(*entry);
        entry->connection = nullptr;
    }
}

async::Task<common::IoErr> QuicSendScheduler::flush_connection(QuicConnection &connection) noexcept {
    FIBER_ASSERT(endpoint_ != nullptr);
    FIBER_ASSERT(socket_ != nullptr);
    FIBER_ASSERT(connection.send_queue_entry.link.linked());

    std::size_t packets_for_connection = 0;
    for (;;) {
        QuicSendDatagram datagram{};
        datagram.data = endpoint_->send_buffer_.get();
        datagram.capacity = options_.send_buffer_size;

        auto built = endpoint_->build_send_datagram(connection, datagram);
        if (!built) {
            co_return built.error();
        }

        if (built->status == QuicBuildSendStatus::NoWork || built->status == QuicBuildSendStatus::Closed ||
            built->status == QuicBuildSendStatus::Blocked) {
            remove(connection);
            co_return common::IoErr::None;
        }

        auto sent = socket_->try_send_packet(datagram.spec);
        if (!sent) {
            endpoint_->rollback_send_datagram(connection, datagram);
            if (sent.error() == common::IoErr::MessageTooLarge && datagram.mtu_probe && datagram.path != nullptr) {
                const QuicTime now = loop_ != nullptr ? quic_time_ms(loop_->now()) : QuicTime{0};
                auto handled = connection.paths().handle_mtu_probe_send_failed(*datagram.path, now);
                if (!handled) {
                    co_return handled.error();
                }
                if (*handled) {
                    continue;
                }
            }
            if (sent.error() == common::IoErr::WouldBlock) {
                auto writable = co_await socket_->wait_writable();
                if (!writable) {
                    co_return writable.error();
                }
                continue;
            }
            co_return sent.error();
        }

        endpoint_->commit_send_datagram(connection, datagram);
        packets_for_connection += datagram.packet_count;
        if (packets_for_connection >= options_.max_packets_per_connection) {
            if (endpoint_->connection_has_send_work(connection)) {
                rotate_front_to_back(connection);
            } else {
                remove(connection);
            }
            co_return common::IoErr::None;
        }
        if (!endpoint_->connection_has_send_work(connection)) {
            remove(connection);
            co_return common::IoErr::None;
        }
    }
}

} // namespace fiber::quic
