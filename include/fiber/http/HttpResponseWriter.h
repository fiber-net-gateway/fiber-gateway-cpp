#ifndef FIBER_HTTP_HTTP_RESPONSE_WRITER_H
#define FIBER_HTTP_HTTP_RESPONSE_WRITER_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "../async/Task.h"
#include "../common/IoError.h"
#include "../common/mem/IoBufChain.h"
#include "HttpExchangeIo.h"

namespace fiber::http {

class HttpExchange;

// A non-owning response sink. Applications can explicitly decorate this value
// without changing HttpExchange or the protocol-specific HttpExchangeIo.
class HttpResponseWriter {
public:
    struct Ops {
        async::Task<common::IoResult<void>> (*send_header)(void *ctx, const OutgoingHeaderBlockView &header,
                                                           std::chrono::milliseconds timeout);
        async::Task<common::IoResult<std::size_t>> (*write_all_chain)(void *ctx, mem::IoBufChain chunk,
                                                                      std::chrono::milliseconds timeout) noexcept;
        async::Task<common::IoResult<std::size_t>> (*write_all_bytes)(void *ctx, const std::uint8_t *buf,
                                                                      std::size_t len, bool end,
                                                                      std::chrono::milliseconds timeout) noexcept;
        async::Task<common::IoResult<std::size_t>> (*write_chain)(void *ctx, mem::IoBufChain &chunk,
                                                                  std::chrono::milliseconds timeout) noexcept;
        async::Task<common::IoResult<std::size_t>> (*write_bytes)(void *ctx, const std::uint8_t *buf, std::size_t len,
                                                                  bool end, std::chrono::milliseconds timeout) noexcept;
        async::Task<common::IoResult<void>> (*flush)(void *ctx, std::chrono::milliseconds timeout) noexcept;
        common::IoResult<void> (*abort)(void *ctx, common::IoErr reason) noexcept;
    };

    HttpResponseWriter() noexcept = default;
    HttpResponseWriter(void *ctx, const Ops &ops) noexcept : ctx_(ctx), ops_(&ops) {}

    [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr && ops_ != nullptr; }

    async::Task<common::IoResult<void>>
    send_header(const OutgoingHeaderBlockView &header,
                std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const;
    async::Task<common::IoResult<std::size_t>>
    write_all(mem::IoBufChain chunk,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept;
    async::Task<common::IoResult<std::size_t>>
    write_all(const std::uint8_t *buf, std::size_t len, bool end,
              std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept;
    async::Task<common::IoResult<std::size_t>>
    write(mem::IoBufChain &chunk, std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept;
    async::Task<common::IoResult<std::size_t>>
    write(const std::uint8_t *buf, std::size_t len, bool end,
          std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept;
    // Publishes bytes retained by response decorators without ending the body.
    // The exchange-backed writer is already write-through, so its implementation is a no-op.
    async::Task<common::IoResult<void>>
    flush(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) const noexcept;
    common::IoResult<void> abort(common::IoErr reason = common::IoErr::Canceled) const noexcept;

private:
    void *ctx_ = nullptr;
    const Ops *ops_ = nullptr;
};

HttpResponseWriter make_http_response_writer(HttpExchange &exchange) noexcept;

} // namespace fiber::http

#endif // FIBER_HTTP_HTTP_RESPONSE_WRITER_H
