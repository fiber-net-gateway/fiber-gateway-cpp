#include "QuicStreamRecvQueue.h"

#include <algorithm>
#include <cstring>
#include <expected>
#include <limits>

#include "../common/Assert.h"
#include "../event/EventLoop.h"

namespace fiber::quic {

static constexpr std::size_t kQuicStreamRecvMaxActiveExtents = 4096;
static constexpr std::size_t kQuicStreamRecvMaxActiveBlocks = 1024;
static constexpr unsigned kBlockSizeShift = 16;
static constexpr std::uint64_t kBlockOffsetMask = 64 * 1024 - 1;
static constexpr std::uint64_t kQuicMaxFlowControlLimit = (1ULL << 62U) - 1U;

class QuicStreamRecvQueue::ReadAwaiter {
public:
    ReadAwaiter(QuicStreamRecvQueue &queue, std::chrono::steady_clock::time_point deadline) noexcept :
        queue_(&queue), deadline_(deadline) {}

    ReadAwaiter(const ReadAwaiter &) = delete;
    ReadAwaiter &operator=(const ReadAwaiter &) = delete;
    ReadAwaiter(ReadAwaiter &&) = delete;
    ReadAwaiter &operator=(ReadAwaiter &&) = delete;

    ~ReadAwaiter() {
        cancel_timer();
        if (queue_ != nullptr) {
            queue_->cancel_read_waiter(this);
        }
    }

    bool await_ready() noexcept {
        if (queue_ == nullptr || queue_->can_take_now()) {
            return true;
        }
        if (timed_out(std::chrono::steady_clock::now())) {
            result_ = common::IoErr::TimedOut;
            completed_ = true;
            return true;
        }
        return false;
    }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        if (queue_ == nullptr || queue_->can_take_now()) {
            return false;
        }
        loop_ = event::EventLoop::current_or_null();
        FIBER_ASSERT(loop_ != nullptr);
        if (timed_out(loop_->now())) {
            result_ = common::IoErr::TimedOut;
            completed_ = true;
            loop_ = nullptr;
            return false;
        }
        if (queue_->read_waiter_ != nullptr) {
            result_ = common::IoErr::Busy;
            completed_ = true;
            loop_ = nullptr;
            return false;
        }

        handle_ = handle;
        queue_->read_waiter_ = this;
        arm_timer();
        return true;
    }

    common::IoErr await_resume() noexcept {
        common::IoErr result = result_;
        cancel_timer();
        if (queue_ != nullptr && queue_->read_waiter_ == this) {
            queue_->read_waiter_ = nullptr;
        }
        queue_ = nullptr;
        loop_ = nullptr;
        handle_ = {};
        result_ = common::IoErr::None;
        resume_posted_ = false;
        completed_ = false;
        return result;
    }

    [[nodiscard]] bool should_resume() const noexcept { return queue_ == nullptr || queue_->can_take_now(); }

    void complete(common::IoErr result) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        result_ = result;
        cancel_timer();
        post_resume();
    }

private:
    [[nodiscard]] bool has_timer() const noexcept { return deadline_ != std::chrono::steady_clock::time_point::max(); }

    [[nodiscard]] bool timed_out(std::chrono::steady_clock::time_point now) const noexcept {
        return has_timer() && now >= deadline_;
    }

    void arm_timer() noexcept {
        if (!has_timer() || loop_ == nullptr) {
            return;
        }
        loop_->post_at<ReadAwaiter, &ReadAwaiter::timer_entry_, &ReadAwaiter::on_timeout>(deadline_, *this);
    }

    void cancel_timer() noexcept {
        if (loop_ != nullptr && timer_entry_.is_in_heap()) {
            loop_->cancel<ReadAwaiter, &ReadAwaiter::timer_entry_>(*this);
        }
    }

    static void on_notify(ReadAwaiter *awaiter) noexcept {
        if (awaiter == nullptr) {
            return;
        }
        awaiter->resume_posted_ = false;
        auto handle = awaiter->handle_;
        awaiter->handle_ = {};
        if (handle) {
            handle.resume();
        }
    }

    static void on_timeout(ReadAwaiter *awaiter) noexcept {
        if (awaiter == nullptr) {
            return;
        }
        awaiter->complete(common::IoErr::TimedOut);
    }

    void post_resume() noexcept {
        if (resume_posted_ || loop_ == nullptr) {
            return;
        }
        resume_posted_ = true;
        loop_->post<ReadAwaiter, &ReadAwaiter::notify_entry_, &ReadAwaiter::on_notify>(*this);
    }

    QuicStreamRecvQueue *queue_ = nullptr;
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
    event::EventLoop *loop_ = nullptr;
    std::coroutine_handle<> handle_{};
    event::EventLoop::NotifyEntry notify_entry_{};
    event::EventLoop::TimerEntry timer_entry_{};
    common::IoErr result_ = common::IoErr::None;
    bool resume_posted_ = false;
    bool completed_ = false;
};

QuicStreamRecvQueue::QuicStreamRecvQueue(mem::IoBufNodePool &pool) noexcept : QuicStreamRecvQueue(pool, Options{}) {}

QuicStreamRecvQueue::QuicStreamRecvQueue(mem::IoBufNodePool &pool, Options options) noexcept :
    pool_(&pool), buffer_limit_(options.buffer_limit), low_water_(std::min(options.low_water, options.buffer_limit)),
    max_stream_data_(options.max_stream_data) {}

void QuicStreamRecvQueue::init(mem::IoBufNodePool &pool) noexcept { init(pool, Options{}); }

void QuicStreamRecvQueue::init(mem::IoBufNodePool &pool, Options options) noexcept {
    FIBER_ASSERT(pool_ == nullptr);
    FIBER_ASSERT(head_ == nullptr);
    FIBER_ASSERT(tail_ == nullptr);
    FIBER_ASSERT(last_insert_ == nullptr);
    FIBER_ASSERT(buffered_bytes_ == 0);
    FIBER_ASSERT(active_extent_count_ == 0);
    FIBER_ASSERT(active_block_count_ == 0);
    FIBER_ASSERT(read_waiter_ == nullptr);
    pool_ = &pool;
    buffer_limit_ = options.buffer_limit;
    low_water_ = std::min(options.low_water, options.buffer_limit);
    max_stream_data_ = options.max_stream_data;
}

QuicStreamRecvQueue::~QuicStreamRecvQueue() {
    FIBER_ASSERT(read_waiter_ == nullptr);
    clear_buffered_extents();
}

common::IoResult<std::size_t> QuicStreamRecvQueue::recv_stream_data(std::uint64_t offset, QuicSlice data,
                                                                    bool fin) noexcept {
    if ((data.data == nullptr && data.len != 0) || offset > std::numeric_limits<std::uint64_t>::max() - data.len)
            [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t end = offset + data.len;
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

    auto checked = check_insert_limits(offset, data.len);
    if (!checked) [[unlikely]] {
        return std::unexpected(checked.error());
    }

    auto inserted = insert_reassembled(offset, data);
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
    clear_buffered_extents();
    notify_read_waiter(common::IoErr::ConnReset);
    return {};
}

void QuicStreamRecvQueue::stop_receiving(std::uint64_t error_code) noexcept {
    if (stop_sending_) {
        return;
    }
    stop_error_code_ = error_code;
    stop_sending_ = true;
    clear_buffered_extents();
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

    auto taken = take_reassembled(max_bytes, out);
    if (!taken) [[unlikely]] {
        return std::unexpected(taken.error());
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
    if (next_read_offset_ >= kQuicMaxFlowControlLimit || buffer_limit_ > kQuicMaxFlowControlLimit - next_read_offset_) {
        return kQuicMaxFlowControlLimit;
    }
    return next_read_offset_ + buffer_limit_;
}

bool QuicStreamRecvQueue::should_extend_max_stream_data() const noexcept {
    if (reset_received_ || stop_sending_ || finished()) {
        return false;
    }
    return max_stream_data_ <= next_read_offset_ || max_stream_data_ - next_read_offset_ < low_water_;
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

common::IoResult<void> QuicStreamRecvQueue::check_insert_limits(std::uint64_t offset, std::size_t len) const noexcept {
    auto cost = insert_cost(offset, len);
    if (!cost) [[unlikely]] {
        return std::unexpected(cost.error());
    }
    if (cost->bytes > buffer_limit_ || buffered_bytes_ > buffer_limit_ - cost->bytes) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    return {};
}

bool QuicStreamRecvQueue::can_take_now() const noexcept {
    return terminal_read_error() != common::IoErr::None ||
           (head_ != nullptr && head_->offset == next_read_offset_ && head_->buf.readable() != 0) || finished();
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

common::IoResult<QuicStreamRecvQueue::InsertCost> QuicStreamRecvQueue::insert_cost(std::uint64_t offset,
                                                                                   std::size_t len) const noexcept {
    if (offset > std::numeric_limits<std::uint64_t>::max() - len) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t data_end = offset + len;
    if (data_end <= next_read_offset_ || len == 0) {
        return InsertCost{};
    }

    if (offset < next_read_offset_) {
        offset = next_read_offset_;
    }

    InsertCost cost{};
    std::uint64_t cursor = offset;
    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    if (last_insert_ != nullptr && last_insert_->offset <= cursor) {
        prev = last_insert_;
        cur = last_insert_->next;
    }

    while (cursor < data_end) {
        while (cur != nullptr && cur->offset <= cursor) {
            const std::uint64_t cur_end = cur->offset + cur->buf.readable();
            if (cur_end > cursor) {
                cursor = std::min(cur_end, data_end);
                if (cursor >= data_end) {
                    return cost;
                }
            }
            prev = cur;
            cur = cur->next;
        }

        if (prev != nullptr) {
            const std::uint64_t prev_end = prev->offset + prev->buf.readable();
            if (prev_end > cursor) {
                cursor = std::min(prev_end, data_end);
                if (cursor >= data_end) {
                    return cost;
                }
                continue;
            }
        }

        const std::uint64_t next_start = cur != nullptr ? cur->offset : data_end;
        const std::uint64_t hole_end = std::min({next_start, data_end, block_end(cursor)});
        if (hole_end <= cursor) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }

        const std::uint64_t block = block_of(cursor);
        cost.bytes += static_cast<std::size_t>(hole_end - cursor);
        if (!has_same_block_neighbor(prev, cur, block)) {
            ++cost.blocks;
        }
        cursor = hole_end;
    }

    return cost;
}

common::IoResult<std::size_t> QuicStreamRecvQueue::insert_reassembled(std::uint64_t offset, QuicSlice data) noexcept {
    if ((data.data == nullptr && data.len != 0) || offset > std::numeric_limits<std::uint64_t>::max() - data.len)
            [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }

    const std::uint64_t data_end = offset + data.len;
    if (data_end <= next_read_offset_ || data.len == 0) {
        return 0;
    }

    if (offset < next_read_offset_) {
        const auto skip = static_cast<std::size_t>(next_read_offset_ - offset);
        data.data += skip;
        data.len -= skip;
        offset = next_read_offset_;
    }

    const std::uint64_t base_offset = offset;
    std::uint64_t cursor = offset;
    std::size_t copied = 0;
    mem::IoBufNode *prev = nullptr;
    mem::IoBufNode *cur = head_;

    if (last_insert_ != nullptr && last_insert_->offset <= cursor) {
        prev = last_insert_;
        cur = last_insert_->next;
    }

    while (cursor < data_end) {
        while (cur != nullptr && cur->offset <= cursor) {
            const std::uint64_t cur_end = cur->offset + cur->buf.readable();
            if (cur_end > cursor) {
                cursor = std::min(cur_end, data_end);
                if (cursor >= data_end) {
                    return copied;
                }
            }
            prev = cur;
            cur = cur->next;
        }

        if (prev != nullptr) {
            const std::uint64_t prev_end = prev->offset + prev->buf.readable();
            if (prev_end > cursor) {
                cursor = std::min(prev_end, data_end);
                if (cursor >= data_end) {
                    return copied;
                }
                continue;
            }
        }

        const std::uint64_t next_start = cur != nullptr ? cur->offset : data_end;
        const std::uint64_t hole_end = std::min({next_start, data_end, block_end(cursor)});
        if (hole_end <= cursor) [[unlikely]] {
            return std::unexpected(common::IoErr::Invalid);
        }

        const auto *src = data.data + static_cast<std::size_t>(cursor - base_offset);
        const std::size_t hole_len = static_cast<std::size_t>(hole_end - cursor);
        const std::uint64_t block = block_of(cursor);

        auto result = create_extent(cursor, hole_len, prev, cur, src, block);
        if (!result) [[unlikely]] {
            return std::unexpected(result.error());
        }

        mem::IoBufNode *new_ext = *result;
        insert_after(prev, *new_ext);

        if (prev != nullptr && block_of(prev->offset) == block) {
            (void) try_merge_with_next(prev);
        }

        if (prev != nullptr && prev->next != new_ext) {
            (void) try_merge_with_next(prev);
        } else {
            (void) try_merge_with_next(new_ext);
            prev = new_ext;
        }

        cursor = hole_end;
        copied += hole_len;
        cur = prev->next;
        last_insert_ = prev;
    }

    return copied;
}

common::IoResult<std::size_t> QuicStreamRecvQueue::take_reassembled(std::size_t max_bytes,
                                                                    mem::IoBufChain &out) noexcept {
    FIBER_ASSERT(&out.node_pool() == pool_);
    std::size_t taken = 0;
    while (max_bytes != 0 && head_ != nullptr && head_->offset == next_read_offset_) {
        mem::IoBufNode *extent = head_;
        const std::size_t readable = extent->buf.readable();
        const std::size_t take_bytes = std::min(readable, max_bytes);

        if (take_bytes == readable) [[likely]] {
            const std::uint64_t next_read = extent->offset + readable;
            buffered_bytes_ -= readable;
            max_bytes -= readable;
            taken += readable;
            if (last_insert_ == extent) {
                last_insert_ = nullptr;
            }
            unlink_after(nullptr, *extent);
            next_read_offset_ = next_read;
            if (!out.append_node(extent)) {
                return std::unexpected(common::IoErr::NoMem);
            }
            continue;
        }

        mem::IoBuf piece = extent->buf.retain_slice(0, take_bytes);
        if (!piece || !out.append(std::move(piece))) {
            return std::unexpected(common::IoErr::NoMem);
        }
        extent->buf.consume(take_bytes);
        extent->offset += take_bytes;
        next_read_offset_ += take_bytes;
        buffered_bytes_ -= take_bytes;
        taken += take_bytes;
        break;
    }

    if (finished()) {
        out.mark_complete();
    }
    return taken;
}

void QuicStreamRecvQueue::clear_buffered_extents() noexcept {
    if (pool_ == nullptr) {
        return;
    }
    mem::IoBufNode *extent = head_;
    while (extent != nullptr) {
        mem::IoBufNode *next = extent->next;
        pool_->release(extent);
        extent = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    last_insert_ = nullptr;
    buffered_bytes_ = 0;
    active_extent_count_ = 0;
    active_block_count_ = 0;
}

std::uint64_t QuicStreamRecvQueue::block_of(std::uint64_t offset) noexcept { return offset >> kBlockSizeShift; }

std::size_t QuicStreamRecvQueue::block_offset(std::uint64_t offset) noexcept {
    return static_cast<std::size_t>(offset & kBlockOffsetMask);
}

std::uint64_t QuicStreamRecvQueue::block_end(std::uint64_t offset) noexcept {
    return (offset & ~kBlockOffsetMask) + kRecvBlockSize;
}

common::IoResult<mem::IoBufNode *> QuicStreamRecvQueue::create_extent(std::uint64_t offset, std::size_t len,
                                                                      mem::IoBufNode *prev, mem::IoBufNode *next,
                                                                      const std::uint8_t *src,
                                                                      std::uint64_t block) noexcept {
    if (len == 0 || src == nullptr) [[unlikely]] {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (active_extent_count_ >= kQuicStreamRecvMaxActiveExtents) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    const bool reuse_prev = prev != nullptr && block_of(prev->offset) == block;
    const bool reuse_next = next != nullptr && block_of(next->offset) == block;
    const bool new_block = !reuse_prev && !reuse_next;
    if (new_block && active_block_count_ >= kQuicStreamRecvMaxActiveBlocks) [[unlikely]] {
        return std::unexpected(common::IoErr::MessageTooLarge);
    }

    mem::IoBufNode *extent = pool_->alloc();
    if (extent == nullptr) [[unlikely]] {
        return std::unexpected(common::IoErr::NoMem);
    }

    const std::size_t local = block_offset(offset);
    mem::IoBuf view{};
    if (reuse_prev || reuse_next) {
        mem::IoBuf &owner = reuse_prev ? prev->buf : next->buf;
        std::memcpy(owner.data() + local, src, len);
        view = owner.retain_storage_slice(local, len);
    } else {
        mem::IoBuf storage = mem::IoBuf::allocate(kRecvBlockSize);
        if (!storage) [[unlikely]] {
            pool_->release(extent);
            return std::unexpected(common::IoErr::NoMem);
        }
        std::memcpy(storage.data() + local, src, len);
        view = storage.retain_storage_slice(local, len);
        ++active_block_count_;
    }

    extent->offset = offset;
    extent->buf = std::move(view);
    extent->next = nullptr;
    ++active_extent_count_;
    buffered_bytes_ += len;
    return extent;
}

void QuicStreamRecvQueue::insert_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
    if (prev == nullptr) {
        extent.next = head_;
        head_ = &extent;
        if (tail_ == nullptr) {
            tail_ = &extent;
        }
        return;
    }

    extent.next = prev->next;
    prev->next = &extent;
    if (tail_ == prev) {
        tail_ = &extent;
    }
}

mem::IoBufNode *QuicStreamRecvQueue::try_merge_with_next(mem::IoBufNode *extent) noexcept {
    if (extent == nullptr || extent->next == nullptr) {
        return extent;
    }

    mem::IoBufNode *right = extent->next;
    if (block_of(extent->offset) != block_of(right->offset) || !extent->buf.try_merge_adjacent(std::move(right->buf))) {
        return extent;
    }

    extent->next = right->next;
    if (tail_ == right) {
        tail_ = extent;
    }
    if (last_insert_ == right) {
        last_insert_ = extent;
    }
    --active_extent_count_;
    pool_->release(right);
    return extent;
}

void QuicStreamRecvQueue::unlink_after(mem::IoBufNode *prev, mem::IoBufNode &extent) noexcept {
    const std::uint64_t block = block_of(extent.offset);
    const bool has_same_block = has_same_block_neighbor(prev, extent.next, block);
    if (prev == nullptr) {
        head_ = extent.next;
    } else {
        prev->next = extent.next;
    }
    if (tail_ == &extent) {
        tail_ = prev;
    }
    if (last_insert_ == &extent) {
        last_insert_ = prev;
    }
    if (!has_same_block) {
        --active_block_count_;
    }
    --active_extent_count_;
}

bool QuicStreamRecvQueue::has_same_block_neighbor(const mem::IoBufNode *prev, const mem::IoBufNode *next,
                                                  std::uint64_t block) noexcept {
    return (prev != nullptr && block_of(prev->offset) == block) || (next != nullptr && block_of(next->offset) == block);
}

} // namespace fiber::quic
