#include <fiber/http/Http2ServerConnection.h>

#include <utility>

#include <fiber/common/Assert.h>

namespace fiber::http {

Http2ServerConnection::Http2ServerConnection(Http2Connection::Options options,
                                             ServerRequestFactory &request_factory) noexcept :
    conn_(std::move(options), &request_factory, ServerRequestFactory::ops()) {
    // Armed before start so a connection that closes immediately still resolves
    // wait_closed() joiners.
    close_gate_.arm(conn_);
}

Http2ServerConnection::~Http2ServerConnection() { FIBER_ASSERT(!worker_hook_.linked()); }

common::IoErr Http2ServerConnection::start(std::unique_ptr<HttpTransport> transport) noexcept {
    return conn_.start(std::move(transport));
}

fiber::async::Task<Http2Connection::CloseResult> Http2ServerConnection::wait_closed() noexcept {
    co_return co_await close_gate_.join();
}

void Http2ServerConnection::request_shutdown() noexcept { conn_.shutdown(common::IoErr::Canceled); }

} // namespace fiber::http
