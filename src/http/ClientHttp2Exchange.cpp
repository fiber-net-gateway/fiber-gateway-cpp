#include "ClientHttp2Exchange.h"

#include <utility>

#include "ClientHttp2Request.h"

namespace fiber::http {

ClientHttp2Exchange::ClientHttp2Exchange(Http2Stream::Lease stream) noexcept : stream_(std::move(stream)) {}

fiber::async::Task<common::IoResult<size_t>> ClientHttp2Exchange::write_body(BodyChunk) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<size_t>> ClientHttp2Exchange::write_body(const std::uint8_t *, std::size_t,
                                                                              bool) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<void>> ClientHttp2Exchange::write_trailer(const HttpHeaders &) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<Http2ResponseHead>> ClientHttp2Exchange::read_header(mem::BufPool &pool) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    Http2ResponseHead head(pool);
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<BodyChunk>> ClientHttp2Exchange::read_body(std::size_t) noexcept {
    if (!stream_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

void ClientHttp2Exchange::cancel(common::IoErr reason) noexcept {
    if (!stream_) {
        return;
    }
    stream_->close(reason);
}

ClientHttp2Request *ClientHttp2Exchange::request() noexcept {
    return stream_ ? static_cast<ClientHttp2Request *>(stream_->owner()) : nullptr;
}

const ClientHttp2Request *ClientHttp2Exchange::request() const noexcept {
    return stream_ ? static_cast<const ClientHttp2Request *>(stream_->owner()) : nullptr;
}

} // namespace fiber::http
