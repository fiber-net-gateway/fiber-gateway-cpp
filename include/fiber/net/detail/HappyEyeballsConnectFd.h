#ifndef FIBER_NET_DETAIL_HAPPY_EYEBALLS_CONNECT_FD_H
#define FIBER_NET_DETAIL_HAPPY_EYEBALLS_CONNECT_FD_H

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "../../common/Assert.h"
#include "../../common/IoError.h"
#include "../../event/EventLoop.h"
#include "../HappyEyeballs.h"
#include "../IpAddress.h"
#include "ConnectFd.h"
#include "Efd.h"

namespace fiber::net::detail {

template<typename Traits>
class HappyEyeballsConnectFd {
public:
    using Address = typename Traits::Address;
    using ConnectInfant = StreamInfant<Traits>;
    using ConnectResult = std::expected<ConnectInfant, HappyEyeballsConnectError>;

    class ConnectAwaiter;

    [[nodiscard]] static ConnectAwaiter connect(event::EventLoop &loop, std::span<const Address> addresses,
                                                HappyEyeballsOptions options) noexcept {
        return ConnectAwaiter(loop, addresses, options);
    }
};

template<typename Traits>
class HappyEyeballsConnectFd<Traits>::ConnectAwaiter {
public:
    ConnectAwaiter(event::EventLoop &loop, std::span<const Address> addresses, HappyEyeballsOptions options) noexcept :
        loop_(&loop), options_(options) {
        normalize_addresses(addresses);
    }

    ConnectAwaiter(const ConnectAwaiter &) = delete;
    ConnectAwaiter &operator=(const ConnectAwaiter &) = delete;
    ConnectAwaiter(ConnectAwaiter &&) = delete;
    ConnectAwaiter &operator=(ConnectAwaiter &&) = delete;

    ~ConnectAwaiter() {
        if (!waiting_) {
            FIBER_ASSERT(!timer_entry_.is_in_heap());
            FIBER_ASSERT(!stop_entry_.is_registered());
            return;
        }
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        abandon();
    }

    bool await_ready() noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        handle_ = handle;

        if (validation_error_ != common::IoErr::None) {
            result_ = std::unexpected(make_error(validation_error_));
            completed_ = true;
            handle_ = {};
            return false;
        }

        waiting_ = true;
        if (!loop_->register_stop<ConnectAwaiter, &ConnectAwaiter::stop_entry_, &ConnectAwaiter::on_loop_stop>(*this)) {
            waiting_ = false;
            result_ = std::unexpected(make_error(common::IoErr::Canceled));
            completed_ = true;
            handle_ = {};
            return false;
        }

        if (options_.total_timeout == std::chrono::milliseconds::max()) {
            deadline_ = TimePoint::max();
        } else {
            deadline_ = add_delay(loop_->now(), options_.total_timeout);
        }

        launch_replacement();
        if (!waiting_) {
            return false;
        }
        suspended_ = true;
        return true;
    }

    ConnectResult await_resume() noexcept { return std::move(result_); }

    [[nodiscard]] bool completed() const noexcept { return completed_; }

    void cancel() noexcept {
        if (!waiting_) {
            return;
        }
        FIBER_ASSERT(loop_ != nullptr);
        FIBER_ASSERT(loop_->in_loop());
        complete_error(common::IoErr::Canceled);
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    struct Attempt {
        Attempt(ConnectAwaiter &connect, std::uint8_t slot_index, std::uint8_t candidate_index) noexcept :
            efd(*connect.loop_, this, &Attempt::on_events), owner(&connect), slot(slot_index),
            candidate(candidate_index) {}

        static void on_events(void *owner_ptr, event::IoEvent events) noexcept {
            auto *attempt = static_cast<Attempt *>(owner_ptr);
            if (attempt == nullptr || attempt->owner == nullptr) {
                return;
            }
            ConnectAwaiter *owner = attempt->owner;
            const std::uint8_t slot = attempt->slot;
            owner->handle_attempt_events(slot, events);
        }

        Efd efd;
        ConnectAwaiter *owner = nullptr;
        std::uint8_t slot = 0;
        std::uint8_t candidate = 0;
    };

    static TimePoint add_delay(TimePoint now, std::chrono::milliseconds delay) noexcept {
        if (delay == std::chrono::milliseconds::max()) {
            return TimePoint::max();
        }
        const auto available = TimePoint::max() - now;
        if (delay >= std::chrono::duration_cast<std::chrono::milliseconds>(available)) {
            return TimePoint::max();
        }
        return now + delay;
    }

    void normalize_addresses(std::span<const Address> addresses) noexcept {
        if (addresses.empty()) {
            validation_error_ = common::IoErr::NotFound;
            return;
        }
        if (addresses.size() > kHappyEyeballsMaxAddresses) {
            validation_error_ = common::IoErr::MessageTooLarge;
            return;
        }
        if (options_.total_timeout < std::chrono::milliseconds::zero() ||
            options_.connection_attempt_delay < std::chrono::milliseconds(10) ||
            options_.max_concurrent_attempts == 0 ||
            options_.max_concurrent_attempts > kHappyEyeballsMaxConcurrentAttempts ||
            options_.first_address_family_count == 0 ||
            options_.first_address_family_count > kHappyEyeballsMaxAddresses) {
            validation_error_ = common::IoErr::Invalid;
            return;
        }
        switch (options_.address_policy) {
            case HappyEyeballsAddressPolicy::V6First:
            case HappyEyeballsAddressPolicy::V4First:
            case HappyEyeballsAddressPolicy::V6Only:
            case HappyEyeballsAddressPolicy::V4Only:
                break;
            default:
                validation_error_ = common::IoErr::Invalid;
                return;
        }

        std::array<std::uint8_t, kHappyEyeballsMaxAddresses> v4{};
        std::array<std::uint8_t, kHappyEyeballsMaxAddresses> v6{};
        std::uint8_t v4_count = 0;
        std::uint8_t v6_count = 0;
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            if (addresses[i].family() == IpFamily::V6) {
                v6[v6_count++] = static_cast<std::uint8_t>(i);
            } else {
                v4[v4_count++] = static_cast<std::uint8_t>(i);
            }
        }

        auto append = [&](std::uint8_t input_index) noexcept {
            addresses_[candidate_count_] = addresses[input_index];
            input_indices_[candidate_count_] = input_index;
            ++candidate_count_;
        };

        if (options_.address_policy == HappyEyeballsAddressPolicy::V6Only) {
            for (std::uint8_t i = 0; i < v6_count; ++i) {
                append(v6[i]);
            }
        } else if (options_.address_policy == HappyEyeballsAddressPolicy::V4Only) {
            for (std::uint8_t i = 0; i < v4_count; ++i) {
                append(v4[i]);
            }
        } else {
            const bool prefer_v6 = options_.address_policy == HappyEyeballsAddressPolicy::V6First;
            const auto &preferred = prefer_v6 ? v6 : v4;
            const auto &alternate = prefer_v6 ? v4 : v6;
            const std::uint8_t preferred_count = prefer_v6 ? v6_count : v4_count;
            const std::uint8_t alternate_count = prefer_v6 ? v4_count : v6_count;
            std::uint8_t preferred_index = 0;
            std::uint8_t alternate_index = 0;
            const std::uint8_t first_count = std::min(options_.first_address_family_count, preferred_count);
            while (preferred_index < first_count) {
                append(preferred[preferred_index++]);
            }
            while (preferred_index < preferred_count || alternate_index < alternate_count) {
                if (alternate_index < alternate_count) {
                    append(alternate[alternate_index++]);
                }
                if (preferred_index < preferred_count) {
                    append(preferred[preferred_index++]);
                }
            }
        }

        if (candidate_count_ == 0) {
            validation_error_ = common::IoErr::NotFound;
        }
    }

    HappyEyeballsConnectError make_error(common::IoErr code) const noexcept {
        HappyEyeballsConnectError error;
        error.code = code;
        error.attempt_errors = attempt_errors_;
        error.input_indices = input_indices_;
        error.attempted_mask = attempted_mask_;
        error.failed_mask = failed_mask_;
        error.candidate_count = candidate_count_;
        error.attempted_count = attempted_count_;
        return error;
    }

    std::uint8_t find_free_slot() const noexcept {
        for (std::uint8_t i = 0; i < attempts_.size(); ++i) {
            if (!attempts_[i].has_value()) {
                return i;
            }
        }
        FIBER_ASSERT(false);
        return 0;
    }

    void record_failure(std::uint8_t candidate, common::IoErr error) noexcept {
        FIBER_ASSERT(candidate < candidate_count_);
        attempt_errors_[candidate] = error == common::IoErr::None ? common::IoErr::Unknown : error;
        failed_mask_ |= static_cast<std::uint16_t>(std::uint16_t{1} << candidate);
    }

    // Returns true only when the attempt remains pending. Immediate success completes the whole
    // operation; immediate failure is recorded so the caller can launch the next candidate now.
    bool launch_one(std::uint8_t candidate) noexcept {
        attempted_mask_ |= static_cast<std::uint16_t>(std::uint16_t{1} << candidate);
        ++attempted_count_;

        auto fd_result = Traits::create_socket(addresses_[candidate]);
        if (!fd_result) {
            record_failure(candidate, fd_result.error());
            return false;
        }

        const std::uint8_t slot = find_free_slot();
        attempts_[slot].emplace(*this, slot, candidate);
        Attempt &attempt = *attempts_[slot];
        common::IoErr attach_error = attempt.efd.attach(*fd_result);
        if (attach_error != common::IoErr::None) {
            ::close(*fd_result);
            attempts_[slot].reset();
            record_failure(candidate, attach_error);
            return false;
        }

        common::IoErr connect_error = Traits::connect_once(attempt.efd.fd(), addresses_[candidate]);
        if (connect_error == common::IoErr::None) {
            complete_success(slot);
            return false;
        }
        if (connect_error != common::IoErr::WouldBlock) {
            attempt.efd.close_fd();
            attempts_[slot].reset();
            record_failure(candidate, connect_error);
            return false;
        }

        common::IoErr watch_error = attempt.efd.watch_add(event::IoEvent::Write);
        if (watch_error != common::IoErr::None) {
            attempt.efd.close_fd();
            attempts_[slot].reset();
            record_failure(candidate, watch_error);
            return false;
        }
        ++active_count_;
        return true;
    }

    bool deadline_reached() const noexcept { return deadline_ != TimePoint::max() && loop_->now() >= deadline_; }

    // An attempt that fails before the stagger expires immediately advances to the next address.
    // Once one remains pending, the normal stagger resumes from that launch time.
    void launch_replacement() noexcept {
        while (waiting_ && next_candidate_ < candidate_count_) {
            if (attempted_count_ != 0 && deadline_reached()) {
                complete_error(common::IoErr::TimedOut);
                return;
            }
            const std::uint8_t candidate = next_candidate_++;
            if (launch_one(candidate)) {
                next_attempt_at_ = add_delay(loop_->now(), options_.connection_attempt_delay);
                if (deadline_reached()) {
                    complete_error(common::IoErr::TimedOut);
                    return;
                }
                break;
            }
        }

        if (!waiting_) {
            return;
        }
        if (next_candidate_ == candidate_count_ && active_count_ == 0) {
            common::IoErr code = attempt_errors_[candidate_count_ - 1];
            complete_error(code == common::IoErr::None ? common::IoErr::Unknown : code);
            return;
        }
        schedule_timer();
    }

    void schedule_timer() noexcept {
        if (timer_entry_.is_in_heap()) {
            loop_->cancel<ConnectAwaiter, &ConnectAwaiter::timer_entry_>(*this);
        }

        TimePoint when = deadline_;
        if (next_candidate_ < candidate_count_ && active_count_ < options_.max_concurrent_attempts) {
            when = std::min(when, next_attempt_at_);
        }
        if (when != TimePoint::max()) {
            loop_->post_at<ConnectAwaiter, &ConnectAwaiter::timer_entry_, &ConnectAwaiter::on_timer>(when, *this);
        }
    }

    static common::IoErr finish_connect(int fd) noexcept {
        int socket_error = 0;
        socklen_t len = sizeof(socket_error);
        for (;;) {
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return common::io_err_from_errno(errno);
        }
        return socket_error == 0 ? common::IoErr::None : common::io_err_from_errno(socket_error);
    }

    void handle_attempt_events(std::uint8_t slot, event::IoEvent events) noexcept {
        if (!waiting_ || slot >= attempts_.size() || !attempts_[slot].has_value() ||
            !event::any(events & event::IoEvent::Write)) {
            return;
        }
        if (deadline_reached()) {
            complete_error(common::IoErr::TimedOut);
            return;
        }

        Attempt &attempt = *attempts_[slot];
        common::IoErr connect_error = finish_connect(attempt.efd.fd());
        if (connect_error == common::IoErr::None) {
            common::IoErr unwatch_error = attempt.efd.unwatch_all();
            if (unwatch_error == common::IoErr::None) {
                complete_success(slot);
                return;
            }
            connect_error = unwatch_error;
        }

        const std::uint8_t candidate = attempt.candidate;
        attempt.efd.close_fd();
        attempts_[slot].reset();
        FIBER_ASSERT(active_count_ > 0);
        --active_count_;
        record_failure(candidate, connect_error);
        launch_replacement();
    }

    void cancel_registrations() noexcept {
        if (timer_entry_.is_in_heap()) {
            loop_->cancel<ConnectAwaiter, &ConnectAwaiter::timer_entry_>(*this);
        }
        if (stop_entry_.is_registered()) {
            loop_->unregister_stop<ConnectAwaiter, &ConnectAwaiter::stop_entry_>(*this);
        }
    }

    void close_attempts() noexcept {
        for (auto &attempt: attempts_) {
            if (attempt.has_value()) {
                attempt->efd.close_fd();
                attempt.reset();
            }
        }
        active_count_ = 0;
    }

    void complete_success(std::uint8_t slot) noexcept {
        FIBER_ASSERT(waiting_);
        FIBER_ASSERT(slot < attempts_.size());
        FIBER_ASSERT(attempts_[slot].has_value());
        Attempt &winner = *attempts_[slot];
        const std::uint8_t candidate = winner.candidate;
        const int fd = winner.efd.release_fd();

        waiting_ = false;
        completed_ = true;
        cancel_registrations();
        result_.emplace(loop_, fd, std::move(addresses_[candidate]));
        close_attempts();
        resume_waiter();
    }

    void complete_error(common::IoErr code) noexcept {
        FIBER_ASSERT(waiting_);
        waiting_ = false;
        completed_ = true;
        cancel_registrations();
        result_ = std::unexpected(make_error(code));
        close_attempts();
        resume_waiter();
    }

    void resume_waiter() noexcept {
        const bool should_resume = suspended_;
        suspended_ = false;
        auto handle = handle_;
        handle_ = {};
        if (should_resume && handle) {
            handle.resume();
        }
    }

    void abandon() noexcept {
        waiting_ = false;
        suspended_ = false;
        handle_ = {};
        cancel_registrations();
        close_attempts();
    }

    static void on_timer(ConnectAwaiter *connect) noexcept {
        if (connect == nullptr || !connect->waiting_) {
            return;
        }
        if (connect->deadline_reached()) {
            connect->complete_error(common::IoErr::TimedOut);
            return;
        }
        if (connect->next_candidate_ < connect->candidate_count_ &&
            connect->active_count_ < connect->options_.max_concurrent_attempts &&
            connect->loop_->now() >= connect->next_attempt_at_) {
            connect->launch_replacement();
            return;
        }
        connect->schedule_timer();
    }

    static void on_loop_stop(ConnectAwaiter *connect) noexcept {
        if (connect != nullptr && connect->waiting_) {
            connect->complete_error(common::IoErr::Canceled);
        }
    }

    event::EventLoop *loop_ = nullptr;
    HappyEyeballsOptions options_{};
    std::array<Address, kHappyEyeballsMaxAddresses> addresses_{};
    std::array<std::uint8_t, kHappyEyeballsMaxAddresses> input_indices_{};
    std::array<common::IoErr, kHappyEyeballsMaxAddresses> attempt_errors_{};
    std::array<std::optional<Attempt>, kHappyEyeballsMaxConcurrentAttempts> attempts_{};
    std::coroutine_handle<> handle_{};
    event::EventLoop::TimerEntry timer_entry_{};
    event::EventLoop::StopEntry stop_entry_{};
    ConnectResult result_{std::unexpected(HappyEyeballsConnectError{})};
    TimePoint deadline_ = TimePoint::max();
    TimePoint next_attempt_at_ = TimePoint::max();
    common::IoErr validation_error_ = common::IoErr::None;
    std::uint16_t attempted_mask_ = 0;
    std::uint16_t failed_mask_ = 0;
    std::uint8_t candidate_count_ = 0;
    std::uint8_t next_candidate_ = 0;
    std::uint8_t attempted_count_ = 0;
    std::uint8_t active_count_ = 0;
    bool waiting_ = false;
    bool suspended_ = false;
    bool completed_ = false;
};

} // namespace fiber::net::detail

#endif // FIBER_NET_DETAIL_HAPPY_EYEBALLS_CONNECT_FD_H
