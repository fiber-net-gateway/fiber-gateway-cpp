#include <gtest/gtest.h>

#include <cstdint>

#include "http/Http2Stream.h"
#include "Http2TestSupport.h"

namespace {

struct OwnedStreamHolder {
    explicit OwnedStreamHolder(std::uint32_t stream_id, bool *destroyed) :
        destroyed_flag(destroyed), stream(stream_id, this, ops()) {}

    static const fiber::http::Http2Stream::Ops &ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kDecoderOps{};
        static const fiber::http::Http2Stream::Ops kOps{
            &OwnedStreamHolder::destroy_owner,
            &OwnedStreamHolder::on_header_block_start,
            &OwnedStreamHolder::on_header_block_complete,
            &OwnedStreamHolder::on_body,
        };
        (void)kDecoderOps;
        return kOps;
    }
    static void destroy_owner(void *owner) noexcept { delete static_cast<OwnedStreamHolder *>(owner); }
    static fiber::common::IoErr on_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &sink) noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kDecoderOps{};
        sink.ctx = nullptr;
        sink.ops = &kDecoderOps;
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_header_block_complete(void *, bool) noexcept {
        return fiber::common::IoErr::None;
    }
    static fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept {
        return fiber::common::IoErr::None;
    }

    ~OwnedStreamHolder() {
        if (destroyed_flag) {
            *destroyed_flag = true;
        }
    }

    bool *destroyed_flag = nullptr;
    fiber::http::Http2Stream stream;
};

} // namespace

TEST(Http2StreamTest, OwnerBackedCreateReturnsUsableLease) {
    fiber::http::Http2Stream::Lease stream = TestHttp2StreamOwner::create(1);
    ASSERT_TRUE(stream);

    stream->close(fiber::common::IoErr::Canceled);
    stream.reset();
}

TEST(Http2StreamTest, EmbeddedOwnerIsDestroyedWhenAdoptedLeaseReleasesClosedStream) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(3, &destroyed);
    fiber::http::Http2Stream::Lease stream = fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    ASSERT_TRUE(stream);

    stream->close(fiber::common::IoErr::Canceled);
    EXPECT_FALSE(destroyed);

    stream.reset();
    EXPECT_TRUE(destroyed);
}

TEST(Http2StreamTest, AdditionalLeaseRetainsEmbeddedOwnerUntilLastReferenceDrops) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(5, &destroyed);
    fiber::http::Http2Stream::Lease initial = fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    fiber::http::Http2Stream::Lease extra = owner->stream.lease();
    ASSERT_TRUE(initial);
    ASSERT_TRUE(extra);

    owner->stream.close(fiber::common::IoErr::Canceled);

    initial.reset();
    EXPECT_FALSE(destroyed);

    extra.reset();
    EXPECT_TRUE(destroyed);
}
