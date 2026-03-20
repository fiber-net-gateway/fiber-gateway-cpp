#ifndef FIBER_HTTP_SERVER_HTTP2_PUSH_H
#define FIBER_HTTP_SERVER_HTTP2_PUSH_H

#include <cstdint>

#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "Http2Stream.h"

namespace fiber::http {

class Http2Connection;

class ServerHttp2Push : public common::NonCopyable, public common::NonMovable {
public:
    [[nodiscard]] static Http2Stream::Lease create(std::uint32_t stream_id, Http2Connection &conn) noexcept;

    [[nodiscard]] Http2Stream &stream() noexcept { return stream_; }
    [[nodiscard]] const Http2Stream &stream() const noexcept { return stream_; }

private:
    explicit ServerHttp2Push(std::uint32_t stream_id, Http2Connection &conn) noexcept;
    static void destroy_owner(void *owner) noexcept;

    [[maybe_unused]] Http2Connection *conn_ = nullptr;
    Http2Stream stream_;
};

} // namespace fiber::http

#endif // FIBER_HTTP_SERVER_HTTP2_PUSH_H
