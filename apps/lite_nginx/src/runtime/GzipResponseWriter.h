#ifndef FIBER_LITE_NGINX_RUNTIME_GZIP_RESPONSE_WRITER_H
#define FIBER_LITE_NGINX_RUNTIME_GZIP_RESPONSE_WRITER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/common/NonCopyable.h>
#include <fiber/common/NonMovable.h>
#include <fiber/common/mem/IoBuf.h>
#include <fiber/http/HttpHeaders.h>
#include <fiber/http/HttpResponseWriter.h>

namespace fiber::http {
class HttpExchange;
}

namespace fiber::lite_nginx::runtime {

struct GzipResponseWriterOptions {
    bool enabled = false;
    bool any_type = false;
    std::span<const std::string> types;
    std::size_t min_length = 20;
    int compression_level = 1;
};

enum class GzipResponseDecision : std::uint8_t {
    Undecided,
    Bypassed,
    Active,
    Completed,
    Failed,
};

struct GzipResponseWriterStats {
    GzipResponseDecision decision = GzipResponseDecision::Undecided;
    std::size_t input_bytes = 0;
    std::size_t output_bytes = 0;
};

class GzipResponseWriter : public fiber::common::NonCopyable, public fiber::common::NonMovable {
public:
    GzipResponseWriter(fiber::http::HttpExchange &exchange, fiber::http::HttpResponseWriter next,
                       const GzipResponseWriterOptions &options) noexcept;
    ~GzipResponseWriter() noexcept;

    [[nodiscard]] fiber::http::HttpResponseWriter writer() noexcept { return writer_; }
    [[nodiscard]] const GzipResponseWriterStats &stats() const noexcept { return stats_; }

private:
    struct CompressionState;

    enum class State : std::uint8_t {
        AwaitingFinalHeader,
        Bypass,
        Active,
        Finished,
        Failed,
    };

    static fiber::async::Task<fiber::common::IoResult<void>>
    on_send_header(void *ctx, const fiber::http::OutgoingHeaderBlockView &header, std::chrono::milliseconds timeout);
    static fiber::async::Task<fiber::common::IoResult<std::size_t>>
    on_write_all_chain(void *ctx, fiber::mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept;
    static fiber::async::Task<fiber::common::IoResult<std::size_t>>
    on_write_all_bytes(void *ctx, const std::uint8_t *buf, std::size_t len, bool end,
                       std::chrono::milliseconds timeout) noexcept;
    static fiber::async::Task<fiber::common::IoResult<std::size_t>>
    on_write_chain(void *ctx, fiber::mem::IoBufChain &chunk, std::chrono::milliseconds timeout) noexcept;
    static fiber::async::Task<fiber::common::IoResult<std::size_t>>
    on_write_bytes(void *ctx, const std::uint8_t *buf, std::size_t len, bool end,
                   std::chrono::milliseconds timeout) noexcept;
    static fiber::async::Task<fiber::common::IoResult<void>> on_flush(void *ctx,
                                                                      std::chrono::milliseconds timeout) noexcept;
    static fiber::common::IoResult<void> on_abort(void *ctx, fiber::common::IoErr reason) noexcept;

    fiber::async::Task<fiber::common::IoResult<void>> send_header(const fiber::http::OutgoingHeaderBlockView &header,
                                                                  std::chrono::milliseconds timeout);
    fiber::async::Task<fiber::common::IoResult<std::size_t>>
    write_all_chain(fiber::mem::IoBufChain chunk, std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<std::size_t>>
    write_all_bytes(const std::uint8_t *buf, std::size_t len, bool end, std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<std::size_t>> write_chain(fiber::mem::IoBufChain &chunk,
                                                                         std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<std::size_t>>
    write_bytes(const std::uint8_t *buf, std::size_t len, bool end, std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> flush(std::chrono::milliseconds timeout) noexcept;

    fiber::async::Task<fiber::common::IoResult<void>> compress_input(const std::uint8_t *buf, std::size_t len,
                                                                     std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> sync_flush(std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> finish(bool end_stream,
                                                             std::chrono::milliseconds timeout) noexcept;
    fiber::async::Task<fiber::common::IoResult<void>> drain_output(bool end_stream,
                                                                   std::chrono::milliseconds timeout) noexcept;
    fiber::common::IoResult<void> initialize_compressor() noexcept;
    fiber::common::IoResult<void> prepare_filtered_headers(const fiber::http::OutgoingHeaderBlockView &header,
                                                           bool transform, bool add_vary) noexcept;
    fiber::common::IoResult<void> abort(fiber::common::IoErr reason) noexcept;
    void fail(fiber::common::IoErr error) noexcept;
    static void *workspace_alloc(void *opaque, unsigned int items, unsigned int size) noexcept;
    static void workspace_free(void *opaque, void *address) noexcept;

    static const fiber::http::HttpResponseWriter::Ops &writer_ops() noexcept;

    fiber::http::HttpExchange *exchange_;
    GzipResponseWriterOptions options_;
    fiber::http::HttpResponseWriter next_;
    fiber::http::HttpResponseWriter writer_;
    fiber::http::HttpHeaders filtered_headers_;
    CompressionState *compression_ = nullptr;
    fiber::mem::IoBuf output_;
    GzipResponseWriterStats stats_;
    State state_ = State::AwaitingFinalHeader;
    std::size_t expected_input_bytes_ = 0;
    std::size_t input_since_yield_ = 0;
    bool has_expected_input_bytes_ = false;
    bool flush_pending_ = false;
};

} // namespace fiber::lite_nginx::runtime

#endif // FIBER_LITE_NGINX_RUNTIME_GZIP_RESPONSE_WRITER_H
