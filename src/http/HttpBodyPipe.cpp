#include <fiber/http/HttpBodyPipe.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <utility>

#include <fiber/event/EventLoop.h>

namespace fiber::http {
namespace {

// Sources hand each read over in the buffer it was received into, sized by
// the read's max_bytes rather than by what arrived. Aggregating trickled
// reads node for node would pin one read-sized block per few bytes until low
// water (~400 MB for 5-byte reads under the 48 KiB default), so a node using
// at most 1/kCoalesceRatio of its capacity is copied into spare room at the
// chain's tail instead. So is a node that small next to a coalesce buffer,
// however exactly sized: kept, it would end the run and send the next sparse
// read to a fresh coalesce buffer. Every node left zero-copy then fills
// 1/kCoalesceRatio of both its own storage and any coalesce buffer opened
// after it, keeping pinned storage within 2 * kCoalesceRatio of the
// buffered bytes plus one coalesce buffer.
constexpr std::size_t kCoalesceRatio = 8;
constexpr std::size_t kCoalesceCapacity = static_cast<std::size_t>(16 * 1024) - mem::kIoBufControlBlockSize;

bool append_coalescing(mem::IoBufChain &dst, mem::IoBufChain &&src, std::size_t coalesce_capacity) noexcept {
    if (dst.complete()) {
        return false;
    }
    const bool complete = src.complete();
    const std::size_t small_bytes = coalesce_capacity / kCoalesceRatio;
    while (mem::IoBufNode *node = src.pop_front_node()) {
        const std::size_t bytes = node->buf.readable();
        if (bytes != 0 && (bytes <= small_bytes || bytes <= node->buf.capacity() / kCoalesceRatio)) {
            mem::IoBuf *tail = dst.back();
            // Only a sole owner may write a buffer's free region: another
            // handle to the storage (a source still receiving into it, a sink
            // retaining it) may claim the same bytes.
            if (tail == nullptr || !tail->unique() || tail->tailroom() < bytes) {
                mem::IoBuf fresh = mem::IoBuf::allocate(std::max(bytes, coalesce_capacity));
                tail = fresh && dst.append(std::move(fresh)) ? dst.back() : nullptr;
            }
            if (tail != nullptr) {
                std::memcpy(tail->writable_data(), node->buf.readable_data(), bytes);
                dst.commit_tailroom(bytes);
                event::EventLoop::current().io_buf_node_pool().release(node);
                continue;
            }
            // Out of memory for a coalesce buffer: keep the node as delivered.
        }
        dst.append_node(node);
    }
    if (complete) {
        dst.mark_complete();
    }
    return true;
}

bool valid_options(const HttpBodyPipeOptions &options) noexcept {
    return options.buffer_size > 0 && options.low_water <= options.buffer_size &&
           options.read_timeout > std::chrono::milliseconds::zero() &&
           options.write_timeout > std::chrono::milliseconds::zero();
}

class HttpBodyPipeAbortGuard {
public:
    HttpBodyPipeAbortGuard(HttpBodyPipeReader source, HttpBodyPipeWriter sink) noexcept :
        source_(source), sink_(sink) {}

    ~HttpBodyPipeAbortGuard() { abort(common::IoErr::Canceled); }

    void abort(common::IoErr reason) noexcept {
        if (!active_) {
            return;
        }
        active_ = false;
        (void) source_.abort(reason);
        (void) sink_.abort(reason);
    }

    void release() noexcept { active_ = false; }

private:
    HttpBodyPipeReader source_;
    HttpBodyPipeWriter sink_;
    bool active_ = true;
};

HttpBodyPipeResult pipe_error(common::IoErr code, HttpBodyPipePhase phase) noexcept {
    return std::unexpected(HttpBodyPipeError{
            .code = code,
            .phase = phase,
    });
}

} // namespace

async::Task<HttpBodyPipeResult> pipe_http_body(HttpBodyPipeReader source, HttpBodyPipeWriter sink,
                                               const HttpBodyPipeOptions &options) noexcept {
    if (!source.valid() || !sink.valid() || !valid_options(options)) {
        co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Validate);
    }

    HttpBodyPipeAbortGuard abort_guard(source, sink);
    mem::IoBufChain buffer;
    mem::IoBufChain refill;
    HttpBodyPipeStats stats;
    std::optional<HttpBodyPipeError> pending_read_error;
    bool input_complete = false;
    bool write_started = false;
    // An unbuffered read is written out before the next one, so it never pins
    // more than itself and copying it would buy nothing.
    const bool coalesce = options.low_water != kUnbufferedBodyPipeLowWater;
    const std::size_t coalesce_capacity = std::min(options.buffer_size, kCoalesceCapacity);

    for (;;) {
        if (!write_started && (!refill.empty() || refill.complete())) {
            if (!buffer.append_chain(std::move(refill))) {
                abort_guard.abort(common::IoErr::Invalid);
                co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Validate);
            }
        }

        // IoBufChain move leaves a valid empty state; reading refill here after
        // the append_chain move (by design: moved-from counts as zero bytes).
        // NOLINTNEXTLINE(bugprone-use-after-move)
        const std::size_t buffered = buffer.readable_bytes() + refill.readable_bytes();
        if (pending_read_error && buffered == 0 && !buffer.complete() && !refill.complete()) {
            abort_guard.abort(pending_read_error->code);
            co_return std::unexpected(*pending_read_error);
        }
        if (input_complete && buffered == 0 && !buffer.complete() && !refill.complete()) {
            abort_guard.release();
            co_return stats;
        }

        const bool can_read =
                options.low_water == kUnbufferedBodyPipeLowWater ? buffered == 0 : buffered < options.low_water;
        if (!input_complete && can_read) {
            const std::size_t capacity = options.buffer_size - buffered;
            ++stats.read_calls;
            auto read_result = co_await source.read(capacity, options.read_timeout);
            if (!read_result) {
                const common::IoErr error = read_result.error();
                if (buffered == 0) {
                    abort_guard.abort(error);
                    co_return pipe_error(error, HttpBodyPipePhase::Read);
                }
                pending_read_error = HttpBodyPipeError{
                        .code = error,
                        .phase = HttpBodyPipePhase::Read,
                };
                input_complete = true;
                continue;
            }

            const std::size_t bytes = read_result->readable_bytes();
            const bool complete = read_result->complete();
            // refill only collects reads made while a write is in flight;
            // otherwise refill is empty and the read lands in buffer directly,
            // where trickled bytes coalesce into its tail.
            mem::IoBufChain &target = write_started ? refill : buffer;
            if (bytes > capacity || (bytes == 0 && !complete) ||
                !(coalesce ? append_coalescing(target, std::move(*read_result), coalesce_capacity)
                           : target.append_chain(std::move(*read_result)))) {
                abort_guard.abort(common::IoErr::Invalid);
                co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Validate);
            }

            input_complete = complete;
            stats.bytes_read += bytes;
            stats.peak_buffered_bytes =
                    std::max(stats.peak_buffered_bytes, buffer.readable_bytes() + refill.readable_bytes());
            continue;
        }

        const std::size_t before_bytes = buffer.readable_bytes();
        const bool before_complete = buffer.complete();
        write_started = true;
        ++stats.write_calls;
        auto write_result = co_await sink.write(buffer, options.write_timeout);
        if (!write_result) {
            const common::IoErr error = write_result.error();
            abort_guard.abort(error);
            co_return pipe_error(error, HttpBodyPipePhase::Write);
        }

        const std::size_t after_bytes = buffer.readable_bytes();
        const std::size_t written = *write_result;
        const bool consumed_expected_bytes = written <= before_bytes && before_bytes - after_bytes == written;
        const bool completion_progress = written == 0 && before_bytes == 0 && before_complete && !buffer.complete();
        const bool completion_preserved = !before_complete || after_bytes == 0 || buffer.complete();
        if (!consumed_expected_bytes || (written == 0 && !completion_progress) || !completion_preserved) {
            abort_guard.abort(common::IoErr::Invalid);
            co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Write);
        }

        if (after_bytes == 0 && !buffer.complete()) {
            if (options.low_water == kUnbufferedBodyPipeLowWater && !before_complete) {
                auto flush_result = co_await sink.flush(options.write_timeout);
                if (!flush_result) {
                    const common::IoErr error = flush_result.error();
                    abort_guard.abort(error);
                    co_return pipe_error(error, HttpBodyPipePhase::Write);
                }
            }
            write_started = false;
        }
        stats.bytes_written += written;
    }
}

} // namespace fiber::http
