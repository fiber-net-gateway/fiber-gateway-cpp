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

void LogContext::attach_loop(event::EventLoop &loop, std::chrono::milliseconds interval) noexcept {
    FIBER_ASSERT(loop.in_loop());
    if (loop_ && loop_ != &loop) {
        detach_loop();
    }
    loop_ = &loop;
    if (!timer_armed_ && interval.count() > 0) {
        arm_timer(interval);
    }
}

void LogContext::arm_timer(std::chrono::milliseconds interval) noexcept {
    FIBER_ASSERT(loop_ != nullptr);
    FIBER_ASSERT(loop_->in_loop());
    FIBER_ASSERT(!timer_armed_);
    loop_->post_at<LogContext, &LogContext::flush_timer_, &LogContext::on_flush_timer>(loop_->now() + interval, *this);
    timer_armed_ = true;
}

void LogContext::detach_loop() noexcept {
    if (!loop_) {
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
    loop_ = nullptr;
}

void LogContext::on_flush_timer(LogContext *context) noexcept {
    context->timer_armed_ = false;
    if (auto *manager = LoggerManager::try_global()) {
        manager->on_context_timer(*context);
    }
}

LogBuffer *LogContext::buffer_at(std::uint16_t index) noexcept {
    if (!buffers_ || index >= buffer_count_) {
        return nullptr;
    }
    return &buffers_[index];
}

} // namespace fiber::log
