#include "HttpBodyPipe.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace fiber::http {
namespace {

bool valid_options(const HttpBodyPipeOptions &options) noexcept {
    return options.buffer_size > 0 && options.low_water > 0 && options.low_water <= options.buffer_size &&
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
                                               mem::IoBufNodePool &node_pool,
                                               const HttpBodyPipeOptions &options) noexcept {
    if (!source.valid() || !sink.valid() || !valid_options(options)) {
        co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Validate);
    }

    HttpBodyPipeAbortGuard abort_guard(source, sink);
    mem::IoBufChain buffer(node_pool);
    mem::IoBufChain refill(node_pool);
    HttpBodyPipeStats stats;
    bool input_complete = false;
    bool write_started = false;

    for (;;) {
        if (!write_started && (!refill.empty() || refill.complete())) {
            if (!buffer.append_chain(std::move(refill))) {
                abort_guard.abort(common::IoErr::Invalid);
                co_return pipe_error(common::IoErr::Invalid, HttpBodyPipePhase::Validate);
            }
        }

        const std::size_t buffered = buffer.readable_bytes() + refill.readable_bytes();
        if (input_complete && buffered == 0 && !buffer.complete() && !refill.complete()) {
            abort_guard.release();
            co_return stats;
        }

        if (!input_complete && buffered < options.low_water) {
            const std::size_t capacity = options.buffer_size - buffered;
            ++stats.read_calls;
            auto read_result = co_await source.read(capacity, options.read_timeout);
            if (!read_result) {
                const common::IoErr error = read_result.error();
                abort_guard.abort(error);
                co_return pipe_error(error, HttpBodyPipePhase::Read);
            }

            const std::size_t bytes = read_result->readable_bytes();
            const bool complete = read_result->complete();
            if (bytes > capacity || (bytes == 0 && !read_result->complete()) ||
                !refill.append_chain(std::move(*read_result))) {
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
            write_started = false;
        }
        stats.bytes_written += written;
    }
}

} // namespace fiber::http
