#include <fiber/log/LogWorker.h>

#include <algorithm>
#include <limits>
#include <sys/syscall.h>
#include <unistd.h>

#include <fiber/common/Assert.h>

namespace fiber::log {
namespace {

std::uint64_t current_thread_id() noexcept {
#if defined(SYS_gettid)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

} // namespace

LogBacklog::LogBacklog(AsyncLogOptions options) noexcept :
    capacity_(options.backlog_capacity), full_policy_(options.full_policy) {}

void LogBacklog::update_peak(std::atomic<std::uint64_t> &peak, std::uint64_t value) noexcept {
    std::uint64_t previous = peak.load(std::memory_order_relaxed);
    while (previous < value &&
           !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void LogBacklog::record_admission(std::size_t bytes) noexcept {
    const std::uint64_t records = queued_records_.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t byte_count = static_cast<std::uint64_t>(bytes);
    const std::uint64_t previous_bytes = queued_bytes_.fetch_add(byte_count, std::memory_order_relaxed);
    const std::uint64_t total_bytes = previous_bytes > std::numeric_limits<std::uint64_t>::max() - byte_count
                                              ? std::numeric_limits<std::uint64_t>::max()
                                              : previous_bytes + byte_count;
    update_peak(peak_queued_records_, records);
    update_peak(peak_queued_bytes_, total_bytes);
}

bool LogBacklog::admit(OwnedLogRecord &record) noexcept {
    const std::size_t bytes = record.allocated_bytes();
    const bool oversized = bytes > capacity_;
    for (;;) {
        const std::uint64_t wake_epoch = wake_epoch_.load(std::memory_order_acquire);
        if (!accepting_.load(std::memory_order_acquire)) {
            dropped_records_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::size_t observed = state_.load(std::memory_order_acquire);
        bool available = false;
        std::size_t desired = observed;
        if (oversized) {
            available = observed == 0;
            desired = kOversizedState;
        } else if (observed != kOversizedState) {
            available = observed <= capacity_ - bytes;
            desired = observed + bytes;
        }

        if (available) {
            if (state_.compare_exchange_weak(observed, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
                if (!accepting_.load(std::memory_order_acquire)) {
                    if (oversized) {
                        state_.store(0, std::memory_order_release);
                    } else {
                        state_.fetch_sub(bytes, std::memory_order_release);
                    }
                    wake_epoch_.fetch_add(1, std::memory_order_release);
                    wake_epoch_.notify_all();
                    dropped_records_.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                record.oversized_admission_ = oversized;
                record_admission(bytes);
                return true;
            }
            continue;
        }
        if (full_policy_ == LogQueueFullPolicy::DropNewest) {
            dropped_records_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        wake_epoch_.wait(wake_epoch, std::memory_order_relaxed);
    }
}

void LogBacklog::release(std::size_t bytes, bool oversized) noexcept {
    queued_records_.fetch_sub(1, std::memory_order_relaxed);
    queued_bytes_.fetch_sub(static_cast<std::uint64_t>(bytes), std::memory_order_relaxed);
    if (oversized) {
        state_.store(0, std::memory_order_release);
    } else {
        state_.fetch_sub(bytes, std::memory_order_release);
    }
    wake_epoch_.fetch_add(1, std::memory_order_release);
    wake_epoch_.notify_all();
}

void LogBacklog::stop_accepting() noexcept {
    accepting_.store(false, std::memory_order_release);
    wake_epoch_.fetch_add(1, std::memory_order_release);
    wake_epoch_.notify_all();
}

void LogBacklog::record_allocation_failure() noexcept { allocation_failures_.fetch_add(1, std::memory_order_relaxed); }

void LogBacklog::record_formatting_failure() noexcept { formatting_failures_.fetch_add(1, std::memory_order_relaxed); }

void LogBacklog::set_writer_thread_id(std::uint64_t id) noexcept {
    writer_thread_id_.store(id, std::memory_order_release);
}

LogQueueStats LogBacklog::stats() const noexcept {
    return {
            .queued_records = queued_records_.load(std::memory_order_relaxed),
            .queued_bytes = queued_bytes_.load(std::memory_order_relaxed),
            .peak_queued_records = peak_queued_records_.load(std::memory_order_relaxed),
            .peak_queued_bytes = peak_queued_bytes_.load(std::memory_order_relaxed),
            .dropped_records = dropped_records_.load(std::memory_order_relaxed),
            .allocation_failures = allocation_failures_.load(std::memory_order_relaxed),
            .formatting_failures = formatting_failures_.load(std::memory_order_relaxed),
            .writer_thread_id = writer_thread_id_.load(std::memory_order_acquire),
            .accepting = accepting_.load(std::memory_order_acquire),
    };
}

LogWorker::LogWorker(std::vector<std::unique_ptr<Appender>> &appenders, AsyncLogOptions options) noexcept :
    appenders_(&appenders), backlog_(options) {}

LogWorker::~LogWorker() {
    if (started_) {
        stop_accepting();
        stop_after_drain();
    }
}

bool LogWorker::start() noexcept {
    event::EventLoop &loop = group_.at(0);
    if (!loop.valid()) {
        return false;
    }

    Control ready{
            .worker = this,
            .kind = ControlKind::Ready,
    };
    loop.post<Control, &Control::notify_entry, &LogWorker::on_control>(ready);
    group_.start();
    ready.done.wait(false, std::memory_order_acquire);
    started_ = ready.result;
    return started_;
}

void LogWorker::submit(OwnedLogRecord &record) noexcept {
    FIBER_ASSERT(started_);
    record.worker_ = this;
    group_.at(0).post<OwnedLogRecord, &OwnedLogRecord::notify_entry_, &LogWorker::on_record>(record);
}

bool LogWorker::run_control(ControlKind kind) noexcept {
    if (!started_) {
        return false;
    }
    FIBER_ASSERT(!group_.at(0).in_loop());
    Control control{
            .worker = this,
            .kind = kind,
    };
    group_.at(0).post<Control, &Control::notify_entry, &LogWorker::on_control>(control);
    control.done.wait(false, std::memory_order_acquire);
    return control.result;
}

bool LogWorker::flush() noexcept { return run_control(ControlKind::Flush); }

bool LogWorker::reopen_all() noexcept { return run_control(ControlKind::Reopen); }

void LogWorker::stop_accepting() noexcept { backlog_.stop_accepting(); }

void LogWorker::stop_after_drain() noexcept {
    if (!started_) {
        return;
    }
    (void) run_control(ControlKind::StopAfterDrain);
    group_.join();
    started_ = false;
}

void LogWorker::record_allocation_failure() noexcept { backlog_.record_allocation_failure(); }

LogQueueStats LogWorker::queue_stats() const noexcept { return backlog_.stats(); }

void LogWorker::process_record(OwnedLogRecord &record) noexcept {
    const std::size_t allocation_size = record.allocated_bytes();
    const bool oversized = record.oversized_admission_;

    FormattedLogRecord formatted;
    if (formatter_.format(record, formatted)) {
        const auto now = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < record.target_count(); ++i) {
            const AppenderId id = record.targets()[i];
            if (id < appenders_->size()) {
                (*appenders_)[id]->append(formatted, now);
            }
        }
        flush_due(now);
        refresh_flush_timer();
    } else {
        backlog_.record_formatting_failure();
        for (std::uint32_t i = 0; i < record.target_count(); ++i) {
            const AppenderId id = record.targets()[i];
            if (id < appenders_->size()) {
                (*appenders_)[id]->record_drop();
            }
        }
    }

    delete &record;
    backlog_.release(allocation_size, oversized);
}

void LogWorker::process_control(Control &control) noexcept {
    switch (control.kind) {
        case ControlKind::Ready:
            backlog_.set_writer_thread_id(current_thread_id());
            control.result = true;
            break;
        case ControlKind::Flush:
            flush_all();
            refresh_flush_timer();
            control.result = true;
            break;
        case ControlKind::Reopen: {
            flush_all();
            bool success = true;
            for (auto &appender: *appenders_) {
                if (!appender->reopen()) {
                    success = false;
                }
            }
            refresh_flush_timer();
            control.result = success;
            break;
        }
        case ControlKind::StopAfterDrain:
            FIBER_ASSERT(stop_control_ == nullptr);
            stop_control_ = &control;
            group_.at(0).post_local<LogWorker, &LogWorker::stop_defer_, &LogWorker::on_stop_deferred>(*this);
            return;
    }
    control.done.store(true, std::memory_order_release);
    control.done.notify_one();
}

void LogWorker::flush_due(std::chrono::steady_clock::time_point now) noexcept {
    for (auto &appender: *appenders_) {
        if (appender->flush_deadline() <= now) {
            appender->flush();
        }
    }
}

void LogWorker::flush_all() noexcept {
    for (auto &appender: *appenders_) {
        appender->flush();
    }
}

void LogWorker::refresh_flush_timer() noexcept {
    event::EventLoop &loop = group_.at(0);
    FIBER_ASSERT(loop.in_loop());
    auto earliest = std::chrono::steady_clock::time_point::max();
    for (const auto &appender: *appenders_) {
        earliest = std::min(earliest, appender->flush_deadline());
    }
    if (flush_timer_armed_ && scheduled_flush_ == earliest) {
        return;
    }
    if (flush_timer_armed_) {
        loop.cancel<LogWorker, &LogWorker::flush_timer_>(*this);
        flush_timer_armed_ = false;
    }
    scheduled_flush_ = earliest;
    if (earliest == std::chrono::steady_clock::time_point::max()) {
        return;
    }
    loop.post_at<LogWorker, &LogWorker::flush_timer_, &LogWorker::on_flush_timer>(earliest, *this);
    flush_timer_armed_ = true;
}

void LogWorker::finish_stop() noexcept {
    event::EventLoop &loop = group_.at(0);
    FIBER_ASSERT(loop.in_loop());
    flush_all();
    if (flush_timer_armed_) {
        loop.cancel<LogWorker, &LogWorker::flush_timer_>(*this);
        flush_timer_armed_ = false;
    }
    scheduled_flush_ = {};
    FIBER_ASSERT(stop_control_ != nullptr);
    Control *control = stop_control_;
    stop_control_ = nullptr;
    control->result = true;
    control->done.store(true, std::memory_order_release);
    control->done.notify_one();
    loop.stop();
}

void LogWorker::on_record(OwnedLogRecord *record) noexcept {
    FIBER_ASSERT(record != nullptr);
    FIBER_ASSERT(record->worker_ != nullptr);
    record->worker_->process_record(*record);
}

void LogWorker::on_control(Control *control) noexcept {
    FIBER_ASSERT(control != nullptr);
    FIBER_ASSERT(control->worker != nullptr);
    control->worker->process_control(*control);
}

void LogWorker::on_flush_timer(LogWorker *worker) noexcept {
    worker->flush_timer_armed_ = false;
    worker->scheduled_flush_ = {};
    const auto now = std::chrono::steady_clock::now();
    worker->flush_due(now);
    worker->refresh_flush_timer();
}

void LogWorker::on_stop_deferred(LogWorker *worker) noexcept { worker->finish_stop(); }

} // namespace fiber::log
