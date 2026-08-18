#ifndef FIBER_HTTP_GZIP_RESPONSE_WRITER_H
#define FIBER_HTTP_GZIP_RESPONSE_WRITER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

namespace fiber::http {

struct GzipResponseWriterOptions {
    bool enabled = false;
    bool any_type = false;
    std::span<const std::string> types;
    std::span<const std::string_view> type_views;
    std::size_t min_length = 20;
    int compression_level = 1;
    // When set, the caller has already applied its protocol-specific
    // Accept-Encoding policy. Leaving it empty preserves the generic
    // gzip-only decision used by lite-nginx.
    std::optional<bool> request_accepts_gzip;
    // Access-server accepts every body-bearing final status; lite-nginx keeps
    // its historical 200/403/404 default when this is false.
    bool all_body_statuses = false;
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

class GzipResponseWriter : public common::NonCopyable, public common::NonMovable {
public:
    GzipResponseWriter(HttpExchange &exchange, HttpResponseWriter next,
                       const GzipResponseWriterOptions &options) noexcept;
    ~GzipResponseWriter() noexcept;

    [[nodiscard]] HttpResponseWriter writer() noexcept { return writer_; }
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

    static async::Task<common::IoResult<void>> on_send_header(void *ctx, const OutgoingHeaderBlockView &header,
                                                              std::chrono::milliseconds timeout);
    static async::Task<common::IoResult<std::size_t>> on_write_all_chain(void *ctx, mem::IoBufChain chunk,
                                                                         std::chrono::milliseconds timeout) noexcept;
    static async::Task<common::IoResult<std::size_t>> on_write_all_bytes(void *ctx, const std::uint8_t *buf,
                                                                         std::size_t len, bool end,
                                                                         std::chrono::milliseconds timeout) noexcept;
    static async::Task<common::IoResult<std::size_t>> on_write_chain(void *ctx, mem::IoBufChain &chunk,
                                                                     std::chrono::milliseconds timeout) noexcept;
    static async::Task<common::IoResult<std::size_t>> on_write_bytes(void *ctx, const std::uint8_t *buf,
                                                                     std::size_t len, bool end,
                                                                     std::chrono::milliseconds timeout) noexcept;
    static async::Task<common::IoResult<void>> on_flush(void *ctx, std::chrono::milliseconds timeout) noexcept;
    static common::IoResult<void> on_abort(void *ctx, common::IoErr reason) noexcept;

    async::Task<common::IoResult<void>> send_header(const OutgoingHeaderBlockView &header,
                                                    std::chrono::milliseconds timeout);
    async::Task<common::IoResult<std::size_t>> write_all_chain(mem::IoBufChain chunk,
                                                               std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<std::size_t>> write_all_bytes(const std::uint8_t *buf, std::size_t len, bool end,
                                                               std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<std::size_t>> write_chain(mem::IoBufChain &chunk,
                                                           std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<std::size_t>> write_bytes(const std::uint8_t *buf, std::size_t len, bool end,
                                                           std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> flush(std::chrono::milliseconds timeout) noexcept;

    async::Task<common::IoResult<void>> compress_input(const std::uint8_t *buf, std::size_t len,
                                                       std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> sync_flush(std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> finish(bool end_stream, std::chrono::milliseconds timeout) noexcept;
    async::Task<common::IoResult<void>> drain_output(bool end_stream, std::chrono::milliseconds timeout) noexcept;
    common::IoResult<void> initialize_compressor() noexcept;
    common::IoResult<void> prepare_filtered_headers(const OutgoingHeaderBlockView &header, bool transform,
                                                    bool add_vary) noexcept;
    common::IoResult<void> abort(common::IoErr reason) noexcept;
    void fail(common::IoErr error) noexcept;
    static void *workspace_alloc(void *opaque, unsigned int items, unsigned int size) noexcept;
    static void workspace_free(void *opaque, void *address) noexcept;

    static const HttpResponseWriter::Ops &writer_ops() noexcept;

    HttpExchange *exchange_;
    GzipResponseWriterOptions options_;
    HttpResponseWriter next_;
    HttpResponseWriter writer_;
    HttpHeaders filtered_headers_;
    CompressionState *compression_ = nullptr;
    mem::IoBuf output_;
    GzipResponseWriterStats stats_;
    State state_ = State::AwaitingFinalHeader;
    std::size_t expected_input_bytes_ = 0;
    std::size_t input_since_yield_ = 0;
    bool has_expected_input_bytes_ = false;
    bool flush_pending_ = false;
};

} // namespace fiber::http

#endif // FIBER_HTTP_GZIP_RESPONSE_WRITER_H
