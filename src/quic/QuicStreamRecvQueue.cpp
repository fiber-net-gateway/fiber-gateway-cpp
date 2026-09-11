#include <fiber/quic/QuicStreamRecvQueue.h>

#include <algorithm>
#include <expected>
#include <limits>
#include <utility>

#include <fiber/async/WaitAwaiter.h>
#include <fiber/common/Assert.h>
#include <fiber/event/EventLoop.h>

namespace fiber::quic {

namespace {

static constexpr std::uint64_t kQuicMaxFlowControlLimit = (1ULL << 62U) - 1U;

QuicDataReassembler::Options reassembler_options(QuicStreamRecvQueue::Options options) noexcept {
    return {
            .buffer_limit = options.buffer_limit,
            .max_active_extents = kQuicStreamRecvMaxActiveExtents,
            .buffer_accounting = QuicDataReassembler::BufferAccounting::AllRetained,
            .storage_budget = options.storage_budget,
    };
}

} // namespace

class QuicStreamRecvQueue::ReadAwaiter : public async::WaitAwaiter {
public:
    ReadAwaiter(QuicStreamRecvQueue &queue, std::chrono::steady_clock::time_point deadline) noexcept :
        WaitAwaiter(deadline, nullptr), queue_(&queue) {}

    // The queue holds a single read_waiter_ slot rather than a list, and it is
    // released here or in await_resume -- whichever the caller reaches -- so
    // complete() has nothing to detach from.
    ~ReadAwaiter() {
        if (queue_ != nullptr) {
            queue_->cancel_read_waiter(this);
        }
    }

    bool await_ready() noexcept {
        if (queue_ == nullptr || queue_->can_take_now()) {
            return true;
        }
        if (timed_out(std::chrono::steady_clock::now())) {
            set_result(common::IoErr::TimedOut);
            mark_completed();
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (queue_ == nullptr || queue_->can_take_now()) {
            return false;
        }
        event::EventLoop *loop = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop != nullptr);
        if (timed_out(loop->now())) {
            set_result(common::IoErr::TimedOut);
            mark_completed();
            return false;
        }
        if (queue_->read_waiter_ != nullptr) {
            set_result(common::IoErr::Busy);
            mark_completed();
            return false;
        }

        queue_->read_waiter_ = this;
        begin_wait(handle, *loop);
        return true;
    }

    common::IoErr await_resume() noexcept {
        const common::IoErr outcome = result();
        end_wait();
        if (queue_ != nullptr && queue_->read_waiter_ == this) {
            queue_->read_waiter_ = nullptr;
        }
        queue_ = nullptr;
        return outcome;
    }

    [[nodiscard]] bool should_resume() const noexcept { return queue_ == nullptr || queue_->can_take_now(); }

private:
    QuicStreamRecvQueue *queue_ = nullptr;
};

QuicStreamRecvQueue::QuicStreamRecvQueue(mem::IoBufNodePool &pool) noexcept : QuicStreamRecvQueue(pool, Options{}) {}

QuicStreamRecvQueue::QuicStreamRecvQueue(mem::IoBufNodePool &pool, Options options) noexcept :
    reassembler_(pool, reassembler_options(options)), low_water_(std::min(options.low_water, options.buffer_limit)),
    max_stream_data_(options.max_stream_data) {}

void QuicStreamRecvQueue::init(mem::IoBufNodePool &pool) noexcept { init(pool, Options{}); }

void QuicStreamRecvQueue::init(mem::IoBufNodePool &pool, Options options) noexcept {
    FIBER_ASSERT(!initialized());
    FIBER_ASSERT(read_waiter_ == nullptr);
    reassembler_.init(pool, reassembler_options(options));
    low_water_ = std::min(options.low_water, options.buffer_limit);
    max_stream_data_ = options.max_stream_data;
}

QuicStreamRecvQueue::~QuicStreamRecvQueue() { FIBER_ASSERT(read_waiter_ == nullptr); }

common::IoResult<std::size_t> QuicStreamRecvQueue::recv_stream_data(std::uint64_t offset, mem::IoBuf data,
                                                                    bool fin) noexcept {
    const std::size_t data_len = data.readable();
    if ((data_len != 0 && !data) || offset > std::numeric_limits<std::uint64_t>::max() - data_len) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t end = offset + data_len;
    if (end > max_stream_data_) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    if (has_final_size_ && (end > received_end_offset_ || (fin && end != received_end_offset_))) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (fin && end < received_end_offset_) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    if (reset_received_ || stop_sending_) {
        received_end_offset_ = std::max(received_end_offset_, end);
        if (fin) {
            auto fixed = set_final_size_from_fin(end);
            if (!fixed) [[unlikely]] {
                return std::unexpected(fixed.error());
            }
        }
        return 0;
    }

    auto inserted = reassembler_.insert(offset, std::move(data));
    if (!inserted) [[unlikely]] {
        return std::unexpected(inserted.error());
    }
    if (fin) {
        auto fixed = set_final_size_from_fin(end);
        if (!fixed) [[unlikely]] {
            return std::unexpected(fixed.error());
        }
    }
    received_end_offset_ = std::max(received_end_offset_, end);

    notify_read_waiter();
    return *inserted;
}

common::IoResult<void> QuicStreamRecvQueue::recv_reset(std::uint64_t error_code, std::uint64_t final_size) noexcept {
    if (received_end_offset_ > final_size) {
        return std::unexpected(common::IoErr::Invalid);
    }
    auto fixed = set_final_size_from_reset(final_size);
    if (!fixed) {
        return std::unexpected(fixed.error());
    }
    reset_error_code_ = error_code;
    reset_received_ = true;
    reassembler_.discard_buffered();
    notify_read_waiter(common::IoErr::ConnReset);
    return {};
}

void QuicStreamRecvQueue::stop_receiving(std::uint64_t error_code) noexcept {
    if (stop_sending_) {
        return;
    }
    stop_error_code_ = error_code;
    stop_sending_ = true;
    reassembler_.discard_buffered();
    notify_read_waiter(common::IoErr::Canceled);
}

common::IoResult<std::size_t> QuicStreamRecvQueue::try_take(std::size_t max_bytes, mem::IoBufChain &out) noexcept {
    const common::IoErr terminal = terminal_read_error();
    if (terminal != common::IoErr::None) {
        return std::unexpected(terminal);
    }
    if (max_bytes == 0) {
        return 0;
    }

    auto taken = reassembler_.take_contiguous(out, max_bytes);
    if (!taken) [[unlikely]] {
        return std::unexpected(taken.error());
    }
    if (finished()) {
        out.mark_complete();
    }
    if (*taken == 0 && !out.complete()) {
        return std::unexpected(common::IoErr::WouldBlock);
    }
    return *taken;
}

async::Task<common::IoResult<std::size_t>> QuicStreamRecvQueue::take(std::size_t max_bytes, mem::IoBufChain &out,
                                                                     std::chrono::milliseconds timeout) noexcept {
    if (timeout < std::chrono::milliseconds::zero()) {
        timeout = std::chrono::milliseconds::zero();
    }

    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool deadline_set = timeout == std::chrono::milliseconds::max();

    for (;;) {
        auto taken = try_take(max_bytes, out);
        if (taken || taken.error() != common::IoErr::WouldBlock) {
            co_return taken;
        }
        if (timeout == std::chrono::milliseconds::zero()) {
            co_return std::unexpected(common::IoErr::TimedOut);
        }
        if (!deadline_set) {
            auto *loop = event::EventLoop::current_or_null();
            FIBER_ASSERT(loop != nullptr);
            deadline = loop->now() + timeout;
            deadline_set = true;
        }

        common::IoErr wait_result = co_await ReadAwaiter(*this, deadline);
        if (wait_result != common::IoErr::None) {
            co_return std::unexpected(wait_result);
        }
    }
}

void QuicStreamRecvQueue::update_max_stream_data(std::uint64_t limit) noexcept {
    if (limit > max_stream_data_) {
        max_stream_data_ = limit;
    }
}

std::uint64_t QuicStreamRecvQueue::next_max_stream_data_limit() const noexcept {
    if (next_read_offset() >= kQuicMaxFlowControlLimit ||
        buffer_limit() > kQuicMaxFlowControlLimit - next_read_offset()) {
        return kQuicMaxFlowControlLimit;
    }
    return next_read_offset() + buffer_limit();
}

bool QuicStreamRecvQueue::should_extend_max_stream_data() const noexcept {
    if (reset_received_ || stop_sending_ || finished()) {
        return false;
    }
    return max_stream_data_ <= next_read_offset() || max_stream_data_ - next_read_offset() < low_water_;
}

common::IoResult<void> QuicStreamRecvQueue::set_final_size_from_fin(std::uint64_t final_size) noexcept {
    if (has_final_size_ && received_end_offset_ != final_size) {
        return std::unexpected(common::IoErr::Invalid);
    }
    received_end_offset_ = final_size;
    has_final_size_ = true;
    fin_received_ = true;
    return {};
}

common::IoResult<void> QuicStreamRecvQueue::set_final_size_from_reset(std::uint64_t final_size) noexcept {
    if (has_final_size_ && received_end_offset_ != final_size) {
        return std::unexpected(common::IoErr::Invalid);
    }
    received_end_offset_ = final_size;
    has_final_size_ = true;
    return {};
}

bool QuicStreamRecvQueue::can_take_now() const noexcept {
    return terminal_read_error() != common::IoErr::None || reassembler_.has_contiguous_data() || finished();
}

common::IoErr QuicStreamRecvQueue::terminal_read_error() const noexcept {
    if (stop_sending_) {
        return common::IoErr::Canceled;
    }
    if (reset_received_) {
        return common::IoErr::ConnReset;
    }
    return common::IoErr::None;
}

void QuicStreamRecvQueue::notify_read_waiter(common::IoErr result) noexcept {
    ReadAwaiter *waiter = read_waiter_;
    if (waiter == nullptr) {
        return;
    }
    if (result != common::IoErr::None || waiter->should_resume()) {
        waiter->complete(result);
    }
}

void QuicStreamRecvQueue::cancel_read_waiter(ReadAwaiter *awaiter) noexcept {
    if (read_waiter_ == awaiter) {
        read_waiter_ = nullptr;
    }
}

} // namespace fiber::quic
