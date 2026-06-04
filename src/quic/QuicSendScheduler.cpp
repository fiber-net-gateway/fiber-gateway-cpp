#include "QuicSendScheduler.h"

#include <expected>
#include <new>

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

class QuicSendScheduler::DeferAwaiter {
public:
    explicit DeferAwaiter(event::EventLoop &loop) noexcept : loop_(&loop) {}
    DeferAwaiter(const DeferAwaiter &) = delete;
    DeferAwaiter &operator=(const DeferAwaiter &) = delete;
    DeferAwaiter(DeferAwaiter &&) = delete;
    DeferAwaiter &operator=(DeferAwaiter &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        handle_ = handle;
        loop_->post_local<DeferAwaiter, &DeferAwaiter::entry_, &DeferAwaiter::on_defer>(*this);
        return true;
    }

    void await_resume() noexcept { handle_ = {}; }

private:
    static void on_defer(DeferAwaiter *awaiter) noexcept {
        if (!awaiter) {
            return;
        }
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::DeferEntry entry_{};
};

QuicSendScheduler::QuicSendScheduler() noexcept = default;

QuicSendScheduler::~QuicSendScheduler() {
    close();
    FIBER_ASSERT(!running_);
}

common::IoResult<void> QuicSendScheduler::init(event::EventLoop &loop, net::UdpSocket &socket,
                                               QuicUdpEndpoint &endpoint, const Options &options) noexcept {
    if (initialized_ || options.send_buffer_size == 0 || options.max_packets_per_wakeup == 0 ||
        options.max_packets_per_connection == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    send_buffer_ = std::make_unique<std::uint8_t[]>(options.send_buffer_size);
    if (!send_buffer_) {
        return std::unexpected(common::IoErr::NoMem);
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

void QuicSendScheduler::submit_after(QuicConnection &connection, std::chrono::milliseconds delay) noexcept {
    if (!initialized_ || closing_) {
        return;
    }
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());

    if (delay <= std::chrono::milliseconds{0}) {
        submit(connection);
        return;
    }

    enqueue_delayed(connection, loop_->now() + delay);
    arm_delay_timer();
}

void QuicSendScheduler::remove(QuicConnection &connection) noexcept {
    auto &index = connection.send_index;
    if (index.link.linked()) {
        if (index.state == QuicConnection::SendIndex::State::Ready) {
            ready_.erase(index);
        } else if (index.state == QuicConnection::SendIndex::State::Delayed) {
            delayed_.erase(index);
        }
    }
    if (blocked_connection_ == &connection) {
        blocked_connection_ = nullptr;
    }
    index.connection = nullptr;
    index.state = QuicConnection::SendIndex::State::None;
    if (initialized_ && loop_ != nullptr && loop_->in_loop()) {
        arm_delay_timer();
    }
}

void QuicSendScheduler::close(common::IoErr reason) noexcept {
    if (!initialized_) {
        return;
    }
    if (stop_reason_ == common::IoErr::None) {
        stop_reason_ = reason;
    }
    closing_ = true;
    blocked_connection_ = nullptr;
    clear_ready();
    clear_delayed();
    if (delay_timer_.is_in_heap()) {
        loop_->cancel<QuicSendScheduler, &QuicSendScheduler::delay_timer_>(*this);
    }
    notify_waiter();
}

async::Task<void> QuicSendScheduler::run() noexcept {
    auto *current = event::EventLoop::current_or_null();
    FIBER_ASSERT(current != nullptr);
    FIBER_ASSERT(loop_ == current);
    FIBER_ASSERT(!running_);

    running_ = true;
    while (!closing_) {
        promote_due_delayed();
        if (!has_work()) {
            arm_delay_timer();
            co_await WaitForWorkAwaiter(*this);
            continue;
        }

        std::size_t packets_this_wakeup = 0;
        while (!closing_ && has_work()) {
            QuicConnection *connection = blocked_connection_;
            if (connection == nullptr) {
                connection = pop_ready();
            }
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
                co_await DeferAwaiter(*loop_);
                packets_this_wakeup = 0;
            }
        }
    }

    running_ = false;
}

bool QuicSendScheduler::should_wake_waiter() const noexcept { return closing_ || has_work() || has_due_delayed(); }

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

bool QuicSendScheduler::has_work() const noexcept { return blocked_connection_ != nullptr || !ready_.empty(); }

bool QuicSendScheduler::has_due_delayed() const noexcept {
    if (loop_ == nullptr) {
        return false;
    }
    const auto now = loop_->now();
    const QuicConnection::SendIndex *index = delayed_.front();
    while (index != nullptr) {
        if (index->ready_at <= now) {
            return true;
        }
        index = delayed_.next_of(*index);
    }
    return false;
}

void QuicSendScheduler::promote_due_delayed() noexcept {
    if (loop_ == nullptr) {
        return;
    }

    const auto now = loop_->now();
    QuicConnection::SendIndex *index = delayed_.front();
    while (index != nullptr) {
        QuicConnection::SendIndex *next = delayed_.next_of(*index);
        if (index->ready_at <= now) {
            delayed_.erase(*index);
            index->state = QuicConnection::SendIndex::State::None;
            if (index->connection != nullptr) {
                enqueue_ready(*index->connection);
            }
        }
        index = next;
    }

    arm_delay_timer();
}

void QuicSendScheduler::arm_delay_timer() noexcept {
    if (!initialized_ || closing_ || loop_ == nullptr || !loop_->in_loop()) {
        return;
    }

    if (delay_timer_.is_in_heap()) {
        loop_->cancel<QuicSendScheduler, &QuicSendScheduler::delay_timer_>(*this);
    }

    QuicConnection::SendIndex *best = nullptr;
    QuicConnection::SendIndex *index = delayed_.front();
    while (index != nullptr) {
        if (best == nullptr || index->ready_at < best->ready_at) {
            best = index;
        }
        index = delayed_.next_of(*index);
    }

    if (best != nullptr) {
        loop_->post_at<QuicSendScheduler, &QuicSendScheduler::delay_timer_, &QuicSendScheduler::on_delay_timer>(
                best->ready_at, *this);
    }
}

void QuicSendScheduler::enqueue_ready(QuicConnection &connection) noexcept {
    auto &index = connection.send_index;
    if (index.state == QuicConnection::SendIndex::State::Delayed) {
        delayed_.erase(index);
        index.state = QuicConnection::SendIndex::State::None;
        arm_delay_timer();
    }
    if (index.state != QuicConnection::SendIndex::State::None) {
        return;
    }
    index.connection = &connection;
    index.state = QuicConnection::SendIndex::State::Ready;
    ready_.push_back(index);
}

void QuicSendScheduler::enqueue_delayed(QuicConnection &connection,
                                        std::chrono::steady_clock::time_point ready_at) noexcept {
    auto &index = connection.send_index;
    if (index.state == QuicConnection::SendIndex::State::Ready ||
        index.state == QuicConnection::SendIndex::State::Inflight) {
        return;
    }

    index.connection = &connection;
    index.ready_at = ready_at;
    if (index.state == QuicConnection::SendIndex::State::Delayed) {
        return;
    }

    index.state = QuicConnection::SendIndex::State::Delayed;
    delayed_.push_back(index);
}

QuicConnection *QuicSendScheduler::pop_ready() noexcept {
    auto *index = ready_.front();
    if (!index) {
        return nullptr;
    }
    ready_.erase(*index);
    QuicConnection *connection = index->connection;
    index->state = QuicConnection::SendIndex::State::Inflight;
    return connection;
}

void QuicSendScheduler::clear_ready() noexcept {
    while (!ready_.empty()) {
        auto *index = ready_.front();
        ready_.erase(*index);
        index->state = QuicConnection::SendIndex::State::None;
    }
}

void QuicSendScheduler::clear_delayed() noexcept {
    while (!delayed_.empty()) {
        auto *index = delayed_.front();
        delayed_.erase(*index);
        index->state = QuicConnection::SendIndex::State::None;
    }
}

void QuicSendScheduler::on_delay_timer(QuicSendScheduler *scheduler) noexcept {
    if (scheduler == nullptr || scheduler->closing_) {
        return;
    }
    scheduler->promote_due_delayed();
    scheduler->notify_waiter();
}

async::Task<common::IoErr> QuicSendScheduler::flush_connection(QuicConnection &connection) noexcept {
    FIBER_ASSERT(endpoint_ != nullptr);
    FIBER_ASSERT(socket_ != nullptr);

    std::size_t packets_for_connection = 0;
    for (;;) {
        QuicSendDatagram datagram{};
        datagram.data = send_buffer_.get();
        datagram.capacity = options_.send_buffer_size;

        auto built = endpoint_->build_send_datagram(connection, datagram);
        if (!built) {
            connection.send_index.state = QuicConnection::SendIndex::State::None;
            co_return built.error();
        }

        if (built->status == QuicBuildSendStatus::Delayed) {
            connection.send_index.state = QuicConnection::SendIndex::State::None;
            submit_after(connection, built->delay);
            co_return common::IoErr::None;
        }

        if (built->status == QuicBuildSendStatus::NoWork || built->status == QuicBuildSendStatus::Closed ||
            built->status == QuicBuildSendStatus::Blocked) {
            connection.send_index.state = QuicConnection::SendIndex::State::None;
            co_return common::IoErr::None;
        }

        auto sent = socket_->try_send_packet(datagram.spec);
        if (!sent) {
            endpoint_->rollback_send_datagram(connection, datagram);
            if (sent.error() == common::IoErr::WouldBlock) {
                blocked_connection_ = &connection;
                auto writable = co_await socket_->wait_writable();
                blocked_connection_ = nullptr;
                if (!writable) {
                    connection.send_index.state = QuicConnection::SendIndex::State::None;
                    co_return writable.error();
                }
                continue;
            }
            connection.send_index.state = QuicConnection::SendIndex::State::None;
            co_return sent.error();
        }

        endpoint_->commit_send_datagram(connection, datagram);
        ++packets_for_connection;
        if (packets_for_connection >= options_.max_packets_per_connection) {
            connection.send_index.state = QuicConnection::SendIndex::State::None;
            if (endpoint_->connection_has_send_work(connection)) {
                enqueue_ready(connection);
            }
            co_return common::IoErr::None;
        }
        if (!endpoint_->connection_has_send_work(connection)) {
            connection.send_index.state = QuicConnection::SendIndex::State::None;
            co_return common::IoErr::None;
        }
    }
}

} // namespace fiber::quic
