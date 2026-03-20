#ifndef FIBER_TESTS_HTTP2_TEST_SUPPORT_H
#define FIBER_TESTS_HTTP2_TEST_SUPPORT_H

#include <cstdint>
#include <new>

#include "http/Http2Connection.h"
#include "http/Http2StreamFactory.h"

namespace {

struct TestHttp2StreamOwner {
    explicit TestHttp2StreamOwner(std::uint32_t stream_id) : stream(stream_id, this, &TestHttp2StreamOwner::destroy_owner) {}

    static fiber::http::Http2Stream::Lease create(std::uint32_t stream_id) noexcept {
        auto *owner = new (std::nothrow) TestHttp2StreamOwner(stream_id);
        if (!owner) {
            return {};
        }
        return fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    }

    static void destroy_owner(void *owner) noexcept { delete static_cast<TestHttp2StreamOwner *>(owner); }

    fiber::http::Http2Stream stream;
};

class TestHttp2StreamFactory {
public:
    [[nodiscard]] static const fiber::http::Http2StreamFactoryOps &ops() noexcept {
        static const fiber::http::Http2StreamFactoryOps kOps{
                &TestHttp2StreamFactory::create_local_stream_op,
                &TestHttp2StreamFactory::create_peer_stream_op,
        };
        return kOps;
    }

    [[nodiscard]] fiber::http::Http2Stream::Lease create_local_stream(std::uint32_t stream_id,
                                                                      fiber::http::Http2Connection &) noexcept {
        return TestHttp2StreamOwner::create(stream_id);
    }

    [[nodiscard]] fiber::http::Http2Stream::Lease create_peer_stream(std::uint32_t stream_id,
                                                                     fiber::http::Http2Connection &) noexcept {
        return TestHttp2StreamOwner::create(stream_id);
    }

private:
    static fiber::http::Http2Stream::Lease create_local_stream_op(void *ctx, std::uint32_t stream_id,
                                                                  fiber::http::Http2Connection &conn) noexcept {
        return static_cast<TestHttp2StreamFactory *>(ctx)->create_local_stream(stream_id, conn);
    }

    static fiber::http::Http2Stream::Lease create_peer_stream_op(void *ctx, std::uint32_t stream_id,
                                                                 fiber::http::Http2Connection &conn) noexcept {
        return static_cast<TestHttp2StreamFactory *>(ctx)->create_peer_stream(stream_id, conn);
    }
};

inline TestHttp2StreamFactory &test_http2_stream_factory() noexcept {
    static TestHttp2StreamFactory factory;
    return factory;
}

} // namespace

#endif // FIBER_TESTS_HTTP2_TEST_SUPPORT_H
