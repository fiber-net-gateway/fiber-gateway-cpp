#ifndef FIBER_TESTS_HTTP2_TEST_SUPPORT_H
#define FIBER_TESTS_HTTP2_TEST_SUPPORT_H

#include <cstdint>
#include <new>

#include "http/Http2Connection.h"
#include "http/Http2StreamFactory.h"

namespace {

struct TestHttp2StreamOwner {
    explicit TestHttp2StreamOwner(std::uint32_t stream_id) : stream(stream_id, this, ops()) {}

    static fiber::http::Http2Stream::Lease create(std::uint32_t stream_id) noexcept {
        auto *owner = new (std::nothrow) TestHttp2StreamOwner(stream_id);
        if (!owner) {
            return {};
        }
        return fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    }

    static const fiber::http::Http2Stream::Ops &ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kDecoderOps{
            &TestHttp2StreamOwner::on_indexed_field,
            &TestHttp2StreamOwner::on_indexed_name,
            &TestHttp2StreamOwner::on_name_raw,
            &TestHttp2StreamOwner::on_name_huffman,
            &TestHttp2StreamOwner::on_value_raw,
            &TestHttp2StreamOwner::on_value_huffman,
        };
        static const fiber::http::Http2Stream::Ops kOps{
            &TestHttp2StreamOwner::destroy_owner,
            &TestHttp2StreamOwner::on_header_block_start,
            &TestHttp2StreamOwner::on_header_block_complete,
            &TestHttp2StreamOwner::on_body,
        };
        (void)kDecoderOps;
        return kOps;
    }

    static void destroy_owner(void *owner) noexcept { delete static_cast<TestHttp2StreamOwner *>(owner); }
    static fiber::common::IoErr on_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &sink) noexcept {
        sink.ctx = nullptr;
        sink.ops = &decoder_ops();
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_header_block_complete(void *, bool) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_indexed_field(void *, fiber::http::Http2HpackDecoder::TableEntryView) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_indexed_name(void *, std::string_view, std::uint64_t) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_raw(void *, const std::uint8_t *, std::size_t,
                                            fiber::http::Http2HpackDecoder::NameView &out) noexcept {
        out = {};
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_name_huffman(void *, const std::uint8_t *, std::size_t,
                                                fiber::http::Http2HpackDecoder::NameView &out) noexcept {
        out = {};
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_value_raw(void *, const std::uint8_t *, std::size_t,
                                             std::string_view &out) noexcept {
        out = {};
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_value_huffman(void *, const std::uint8_t *, std::size_t,
                                                 std::string_view &out) noexcept {
        out = {};
        return fiber::common::IoErr::None;
    }
    static const fiber::http::Http2HpackDecoder::Ops &decoder_ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kOps{
            &TestHttp2StreamOwner::on_indexed_field,
            &TestHttp2StreamOwner::on_indexed_name,
            &TestHttp2StreamOwner::on_name_raw,
            &TestHttp2StreamOwner::on_name_huffman,
            &TestHttp2StreamOwner::on_value_raw,
            &TestHttp2StreamOwner::on_value_huffman,
        };
        return kOps;
    }

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
