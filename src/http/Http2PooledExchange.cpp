#include <fiber/http/Http2PooledExchange.h>

namespace fiber::http {
Http2PooledExchange::Http2PooledExchange(Http2ConnectionPoolCore::Lease lease, mem::BufPool &pool) noexcept :
    lease_(std::move(lease)), ex_(lease_.open_exchange(pool)) {}
Http2PooledExchange::Http2PooledExchange(Http2PooledExchange &&other) noexcept :
    lease_(std::move(other.lease_)), ex_(std::move(other.ex_)) {}
Http2PooledExchange &Http2PooledExchange::operator=(Http2PooledExchange &&other) noexcept {
    if (this != &other) {
        reset();
        lease_ = std::move(other.lease_);
        ex_ = std::move(other.ex_);
    }
    return *this;
}
Http2PooledExchange::~Http2PooledExchange() { reset(); }
void Http2PooledExchange::reset() noexcept {
    // Dropping ClientHttp2Exchange alone releases only its reference: an
    // unfinished stream is still owned by the connection's stream table.
    // Cancel it before returning admission capacity to the pool.
    if (auto *stream = ex_.stream();
        stream && stream->attached_to_connection() && !(stream->local_end_stream() && stream->remote_end_stream())) {
        auto aborted = ex_.abort();
        if (!aborted && lease_.valid())
            lease_.connection().shutdown(aborted.error());
    }
    ex_ = ClientHttp2Exchange{};
    lease_.reset();
}
} // namespace fiber::http
