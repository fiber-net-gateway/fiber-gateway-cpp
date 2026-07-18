#include "LogContext.h"

#include <cstdlib>
#include <new>

#include "../common/Assert.h"
#include "LogConfig.h"
#include "LoggerManager.h"

namespace fiber::log {

LogContext::FormatScratchLease::~FormatScratchLease() noexcept {
    if (context_) {
        context_->release_format_scratch();
    }
}

LogContext::~LogContext() {
    if (auto *manager = LoggerManager::try_global()) {
        manager->destroy_context(*this);
    } else {
        reset();
    }
    std::free(format_scratch_);
}

bool LogContext::prepare(std::uint64_t generation, std::uint16_t buffer_count) noexcept {
    if (generation_ == generation && buffer_count_ == buffer_count) {
        return true;
    }
    reset();
    generation_ = generation;
    if (buffer_count == 0) {
        return true;
    }
    buffers_ = new (std::nothrow) LogBuffer[buffer_count]{};
    if (!buffers_) {
        buffer_count_ = buffer_count;
        return false;
    }
    buffer_count_ = buffer_count;
    return true;
}

void LogContext::reset() noexcept {
    FIBER_ASSERT(!format_scratch_in_use_);
    detach_loop();
    if (buffers_) {
        for (std::uint16_t i = 0; i < buffer_count_; ++i) {
            std::free(buffers_[i].data);
        }
        delete[] buffers_;
    }
    buffers_ = nullptr;
    buffer_count_ = 0;
    generation_ = 0;
    if (!format_scratch_) {
        format_scratch_allocation_failed_ = false;
    }
}

LogContext::FormatScratchLease LogContext::acquire_format_scratch() noexcept {
    if (format_scratch_in_use_) {
        return FormatScratchLease();
    }
    if (!format_scratch_ && !format_scratch_allocation_failed_) {
        format_scratch_ = static_cast<char *>(std::malloc(kMaxFormattedLogLineSize));
        if (!format_scratch_) {
            format_scratch_allocation_failed_ = true;
        }
    }
    if (!format_scratch_) {
        return FormatScratchLease();
    }
    format_scratch_in_use_ = true;
    return FormatScratchLease(*this, format_scratch_);
}

void LogContext::release_format_scratch() noexcept {
    FIBER_ASSERT(format_scratch_in_use_);
    format_scratch_in_use_ = false;
}

void LogContext::update_flush_schedule(LogBuffer &changed) noexcept {
    auto *loop = event::EventLoop::current_or_null();
    if (!loop) {
        return;
    }
    if (loop_ && loop_ != loop) {
        detach_loop();
    }
    if (!loop_) {
        loop_ = loop;
    }

    if (!timer_armed_) {
        rebuild_flush_schedule();
        return;
    }
    if (scheduled_buffer_ == &changed) {
        if (changed.size == 0 || changed.flush_at != scheduled_at_) {
            rebuild_flush_schedule();
        }
        return;
    }
    if (changed.size > 0 && changed.flush_at < scheduled_at_) {
        loop_->cancel<LogContext, &LogContext::flush_timer_>(*this);
        timer_armed_ = false;
        arm_timer(changed);
    }
}

void LogContext::rebuild_flush_schedule() noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    if (timer_armed_) {
        loop_->cancel<LogContext, &LogContext::flush_timer_>(*this);
        timer_armed_ = false;
    }

    LogBuffer *earliest = nullptr;
    for (std::uint16_t i = 0; i < buffer_count_; ++i) {
        LogBuffer &buffer = buffers_[i];
        if (buffer.size > 0 && (!earliest || buffer.flush_at < earliest->flush_at)) {
            earliest = &buffer;
        }
    }
    if (!earliest) {
        scheduled_buffer_ = nullptr;
        scheduled_at_ = {};
        loop_ = nullptr;
        return;
    }
    arm_timer(*earliest);
}

void LogContext::arm_timer(LogBuffer &buffer) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!timer_armed_);
    FIBER_ASSERT(buffer.size > 0);
    FIBER_ASSERT(buffer.flush_at != std::chrono::steady_clock::time_point{});
    scheduled_buffer_ = &buffer;
    scheduled_at_ = buffer.flush_at;
    loop_->post_at<LogContext, &LogContext::flush_timer_, &LogContext::on_flush_timer>(scheduled_at_, *this);
    timer_armed_ = true;
}

void LogContext::cancel_flush_schedule() noexcept { detach_loop(); }

void LogContext::detach_loop() noexcept {
    if (!loop_) {
        FIBER_ASSERT(!timer_armed_);
        scheduled_buffer_ = nullptr;
        scheduled_at_ = {};
        return;
    }
    if (timer_armed_) {
        if (loop_->in_loop()) {
            loop_->cancel<LogContext, &LogContext::flush_timer_>(*this);
        } else {
            FIBER_ASSERT(!loop_->running());
            loop_->cancel_quiesced<LogContext, &LogContext::flush_timer_>(*this);
        }
        timer_armed_ = false;
    }
    scheduled_buffer_ = nullptr;
    scheduled_at_ = {};
    loop_ = nullptr;
}

void LogContext::on_flush_timer(LogContext *context) noexcept {
    context->timer_armed_ = false;
    context->scheduled_buffer_ = nullptr;
    context->scheduled_at_ = {};
    if (auto *manager = LoggerManager::try_global()) {
        manager->on_context_timer(*context);
    } else {
        context->loop_ = nullptr;
    }
}

LogBuffer *LogContext::buffer_at(std::uint16_t index) noexcept {
    if (!buffers_ || index >= buffer_count_) {
        return nullptr;
    }
    return &buffers_[index];
}

} // namespace fiber::log
