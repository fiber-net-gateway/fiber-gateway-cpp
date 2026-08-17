#include <fiber/http/HttpResponseWriter.h>

#include <fiber/common/Assert.h>
#include <fiber/http/HttpExchange.h>

namespace fiber::http {
namespace {

async::Task<common::IoResult<void>> exchange_send_header(void *ctx, const OutgoingHeaderBlockView &header,
                                                         std::chrono::milliseconds timeout) {
    co_return co_await static_cast<HttpExchange *>(ctx)->send_header(header, timeout);
}

async::Task<common::IoResult<std::size_t>> exchange_write_all_chain(void *ctx, mem::IoBufChain chunk,
                                                                    std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<HttpExchange *>(ctx)->write_all(std::move(chunk), timeout);
}

async::Task<common::IoResult<std::size_t>> exchange_write_all_bytes(void *ctx, const std::uint8_t *buf, std::size_t len,
                                                                    bool end,
                                                                    std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<HttpExchange *>(ctx)->write_all(buf, len, end, timeout);
}

async::Task<common::IoResult<std::size_t>> exchange_write_chain(void *ctx, mem::IoBufChain &chunk,
                                                                std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<HttpExchange *>(ctx)->write(chunk, timeout);
}

async::Task<common::IoResult<std::size_t>> exchange_write_bytes(void *ctx, const std::uint8_t *buf, std::size_t len,
                                                                bool end, std::chrono::milliseconds timeout) noexcept {
    co_return co_await static_cast<HttpExchange *>(ctx)->write(buf, len, end, timeout);
}

async::Task<common::IoResult<void>> exchange_flush(void *, std::chrono::milliseconds) noexcept {
    co_return common::IoResult<void>{};
}

common::IoResult<void> exchange_abort(void *ctx, common::IoErr reason) noexcept {
    return static_cast<HttpExchange *>(ctx)->abort(reason);
}

const HttpResponseWriter::Ops kExchangeWriterOps{
        &exchange_send_header, &exchange_write_all_chain, &exchange_write_all_bytes, &exchange_write_chain,
        &exchange_write_bytes, &exchange_flush,           &exchange_abort,
};

} // namespace

async::Task<common::IoResult<void>> HttpResponseWriter::send_header(const OutgoingHeaderBlockView &header,
                                                                    std::chrono::milliseconds timeout) const {
    FIBER_ASSERT(valid());
    co_return co_await ops_->send_header(ctx_, header, timeout);
}

async::Task<common::IoResult<std::size_t>>
HttpResponseWriter::write_all(mem::IoBufChain chunk, std::chrono::milliseconds timeout) const noexcept {
    FIBER_ASSERT(valid());
    co_return co_await ops_->write_all_chain(ctx_, std::move(chunk), timeout);
}

async::Task<common::IoResult<std::size_t>>
HttpResponseWriter::write_all(const std::uint8_t *buf, std::size_t len, bool end,
                              std::chrono::milliseconds timeout) const noexcept {
    FIBER_ASSERT(valid());
    co_return co_await ops_->write_all_bytes(ctx_, buf, len, end, timeout);
}

async::Task<common::IoResult<std::size_t>> HttpResponseWriter::write(mem::IoBufChain &chunk,
                                                                     std::chrono::milliseconds timeout) const noexcept {
    FIBER_ASSERT(valid());
    co_return co_await ops_->write_chain(ctx_, chunk, timeout);
}

async::Task<common::IoResult<std::size_t>> HttpResponseWriter::write(const std::uint8_t *buf, std::size_t len, bool end,
                                                                     std::chrono::milliseconds timeout) const noexcept {
    FIBER_ASSERT(valid());
    co_return co_await ops_->write_bytes(ctx_, buf, len, end, timeout);
}

async::Task<common::IoResult<void>> HttpResponseWriter::flush(std::chrono::milliseconds timeout) const noexcept {
    FIBER_ASSERT(valid());
    co_return co_await ops_->flush(ctx_, timeout);
}

common::IoResult<void> HttpResponseWriter::abort(common::IoErr reason) const noexcept {
    FIBER_ASSERT(valid());
    return ops_->abort(ctx_, reason);
}

HttpResponseWriter make_http_response_writer(HttpExchange &exchange) noexcept {
    return {&exchange, kExchangeWriterOps};
}

} // namespace fiber::http
