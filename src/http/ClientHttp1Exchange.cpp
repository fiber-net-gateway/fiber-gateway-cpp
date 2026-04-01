#include "ClientHttp1Exchange.h"

#include <utility>

#include "Http1ClientConnection.h"

namespace fiber::http {

ClientHttp1Exchange::ClientHttp1Exchange(Http1ClientConnection &conn, Http1ClientExchangeOptions options) noexcept
    : conn_(&conn), options_(std::move(options)), response_head_(pool_), response_trailers_(pool_) {
    active_ = conn.acquire_exchange(this);
}

ClientHttp1Exchange::~ClientHttp1Exchange() {
    if (!conn_ || !active_) {
        return;
    }
    if (done()) {
        conn_->release_exchange(this, true);
    } else {
        conn_->fail_exchange(this);
    }
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::send_header(const Http1RequestHead &, bool) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<std::size_t>> ClientHttp1Exchange::write_body(BodyChunk) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<std::size_t>> ClientHttp1Exchange::write_body(const std::uint8_t *buf,
                                                                                   std::size_t len,
                                                                                   bool) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    if (len != 0 && buf == nullptr) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::send_trailer(const HttpHeaders &) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<const Http1ResponseHead *>> ClientHttp1Exchange::read_header() noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<BodyChunk>> ClientHttp1Exchange::read_body(std::size_t) noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

fiber::async::Task<common::IoResult<void>> ClientHttp1Exchange::discard_response_body() noexcept {
    if (!active_) {
        co_return std::unexpected(common::IoErr::Invalid);
    }
    co_return std::unexpected(common::IoErr::NotSupported);
}

} // namespace fiber::http
