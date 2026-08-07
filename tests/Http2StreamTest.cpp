#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>
#include <fiber/http/HttpHeaderHash.h>
#include <fiber/http/Huffman.h>

#define private public
#define protected public
#include <fiber/http/Http2Connection.h>
#include <fiber/http/Http2Stream.h>
#undef private
#undef protected

#include "Http2TestSupport.h"
#include "HttpTransportStub.h"

namespace {

struct OwnedStreamHolder {
    explicit OwnedStreamHolder(bool *destroyed) : destroyed_flag(destroyed), stream(this, ops()) {}

    static const fiber::http::Http2Stream::Ops &ops() noexcept {
        static const fiber::http::Http2Stream::Ops kOps{
                &OwnedStreamHolder::destroy_owner,
                &OwnedStreamHolder::on_header_block_start,
                &OwnedStreamHolder::on_header_block_complete,
                &OwnedStreamHolder::on_body,
                &OwnedStreamHolder::on_abort,
        };
        return kOps;
    }

    static void destroy_owner(void *owner) noexcept { delete static_cast<OwnedStreamHolder *>(owner); }

    static fiber::common::IoErr on_header_block_start(void *owner,
                                                      fiber::http::Http2HpackDecoder::Sink &sink) noexcept {
        sink.ctx = owner;
        sink.ops = &decoder_ops();
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_header_block_complete(void *, bool) noexcept { return fiber::common::IoErr::None; }

    static fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept {
        return fiber::common::IoErr::None;
    }

    static void on_abort(void *, fiber::common::IoErr) noexcept {}

    static fiber::common::IoErr on_indexed_field(void *, fiber::http::Http2HpackDecoder::TableEntryView) noexcept {
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_indexed_name(void *owner,
                                                fiber::http::Http2HpackDecoder::TableEntryView entry) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        self->pending_name_storage.assign(entry.name.data(), entry.name.size());
        self->pending_name_hash = entry.name_hash;
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_name_raw(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        self->pending_name_storage.assign(reinterpret_cast<const char *>(data), len);
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_name_huffman(void *owner, const std::uint8_t *data, std::size_t len) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_storage.assign(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        fiber::http::HpackHuffmanDecodeResult result = fiber::http::hpack_huffman_decode(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_name_storage.data()), decoded_len,
                true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_name_hash = fiber::http::http_header_name_hash(self->pending_name_storage);
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_value_raw(void *owner, const std::uint8_t *data, std::size_t len,
                                             fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        self->pending_value_storage.assign(reinterpret_cast<const char *>(data), len);
        if (out != nullptr) {
            out->name = self->pending_name_storage;
            out->name_hash = self->pending_name_hash;
            out->value = self->pending_value_storage;
        }
        return fiber::common::IoErr::None;
    }

    static fiber::common::IoErr on_value_huffman(void *owner, const std::uint8_t *data, std::size_t len,
                                                 fiber::http::Http2HpackDecoder::FieldView *out) noexcept {
        auto *self = static_cast<OwnedStreamHolder *>(owner);
        bool ok = false;
        const std::size_t decoded_len = fiber::http::hpack_huffman_decoded_length(data, len, &ok);
        if (!ok) {
            return fiber::common::IoErr::Invalid;
        }
        self->pending_value_storage.assign(decoded_len, '\0');
        fiber::http::HpackHuffmanDecodeState state;
        fiber::http::HpackHuffmanDecodeResult result = fiber::http::hpack_huffman_decode(
                state, data, len, reinterpret_cast<std::uint8_t *>(self->pending_value_storage.data()), decoded_len,
                true);
        if (result.code != fiber::http::HpackHuffmanCode::Ok || result.written != decoded_len) {
            return fiber::common::IoErr::Invalid;
        }
        if (out != nullptr) {
            out->name = self->pending_name_storage;
            out->name_hash = self->pending_name_hash;
            out->value = self->pending_value_storage;
        }
        return fiber::common::IoErr::None;
    }

    static const fiber::http::Http2HpackDecoder::Ops &decoder_ops() noexcept {
        static const fiber::http::Http2HpackDecoder::Ops kOps{
                &OwnedStreamHolder::on_indexed_field, &OwnedStreamHolder::on_indexed_name,
                &OwnedStreamHolder::on_name_raw,      &OwnedStreamHolder::on_name_huffman,
                &OwnedStreamHolder::on_value_raw,     &OwnedStreamHolder::on_value_huffman,
        };
        return kOps;
    }

    ~OwnedStreamHolder() {
        if (destroyed_flag) {
            *destroyed_flag = true;
        }
    }

    bool *destroyed_flag = nullptr;
    fiber::http::Http2Stream stream;
    std::string pending_name_storage;
    std::string pending_value_storage;
    std::uint64_t pending_name_hash = 0;
};

class DummyHttpTransport final : public fiber::test::HttpTransportStub {
public:
    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> wait_readable(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read(void *, size_t, std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> read_into(fiber::mem::IoBuf &,
                                                                  std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> readv_into(fiber::mem::IoBufChain &,
                                                                   std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(const void *, size_t,
                                                              std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &, std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &,
                                                               std::chrono::milliseconds) override {
        co_return std::unexpected(fiber::common::IoErr::NotSupported);
    }

    void close() override {}
    [[nodiscard]] bool valid() const noexcept override { return true; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_ ? *loop_ : fallback_loop_; }

private:
    fiber::net::SocketAddress remote_addr_{};
    fiber::event::EventLoop *loop_ = fiber::event::EventLoop::current_or_null();
    mutable fiber::event::EventLoop fallback_loop_{};
};

fiber::http::Http2Connection::Options make_options() {
    fiber::http::Http2Connection::Options options;
    options.initial_connection_recv_window = 65535;
    options.initial_stream_recv_window = 64;
    options.stream_recv_window_low_watermark = 16;
    return options;
}

std::uint32_t parse_window_update_increment(const std::uint8_t *data) {
    return ((static_cast<std::uint32_t>(data[9]) & 0x7fU) << 24) | (static_cast<std::uint32_t>(data[10]) << 16) |
           (static_cast<std::uint32_t>(data[11]) << 8) | static_cast<std::uint32_t>(data[12]);
}

} // namespace

TEST(Http2StreamTest, OwnerBackedCreateReturnsUsableLease) {
    fiber::http::Http2Stream::Lease stream = TestHttp2StreamOwner::create();
    ASSERT_TRUE(stream);

    stream->close(fiber::common::IoErr::Canceled);
    stream.reset();
}

TEST(Http2StreamTest, UpdateSendWindowAllowsNegativeWindowToRecover) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(&destroyed);
    fiber::http::Http2Stream::Lease stream = fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    stream->send_window_ = -5;
    stream->update_send_window(4);
    EXPECT_EQ(stream->send_window(), -1);
    stream->update_send_window(2);
    EXPECT_EQ(stream->send_window(), 1);
    stream->close(fiber::common::IoErr::Canceled);
    stream.reset();
    EXPECT_TRUE(destroyed);
}

TEST(Http2StreamTest, EmbeddedOwnerIsDestroyedWhenAdoptedLeaseReleasesClosedStream) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(&destroyed);
    fiber::http::Http2Stream::Lease stream = fiber::http::Http2Stream::Lease::adopt(&owner->stream);
    ASSERT_TRUE(stream);

    stream->close(fiber::common::IoErr::Canceled);
    EXPECT_FALSE(destroyed);

    stream.reset();
    EXPECT_TRUE(destroyed);
}

TEST(Http2StreamTest, AdditionalLeaseRetainsEmbeddedOwnerUntilLastReferenceDrops) {
    bool destroyed = false;
    auto *owner = new OwnedStreamHolder(&destroyed);
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

TEST(Http2StreamTest, MaybeReplenishRecvWindowEnqueuesWindowUpdateAndTracksRemainingWindow) {
    struct Outcome {
        fiber::common::IoErr result = fiber::common::IoErr::Invalid;
        std::int32_t recv_window_remaining = 0;
        std::size_t pending_control_bytes = 0;
        std::size_t control_bytes = 0;
        std::uint8_t frame_type = 0;
        std::uint32_t window_increment = 0;
        bool stream_created = false;
    };

    fiber::event::EventLoopGroup group(1);
    std::promise<Outcome> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        Outcome outcome;
        fiber::http::Http2Connection connection(make_options(), &test_http2_stream_factory(),
                                                TestHttp2StreamFactory::ops());
        connection.state_ = fiber::http::Http2Connection::State::Running;

        fiber::http::Http2Stream *stream = connection.create_peer_stream(1);
        outcome.stream_created = stream != nullptr;
        if (stream != nullptr) {
            stream->recv_window_remaining_ = 15;
            outcome.result = stream->maybe_replenish_recv_window(15);
            outcome.recv_window_remaining = stream->recv_window_remaining();
            outcome.pending_control_bytes = connection.control_hook_.encoded_.readable_bytes();
            if (fiber::mem::IoBuf *control = connection.control_hook_.encoded_.first_readable()) {
                outcome.control_bytes = control->readable();
                const std::uint8_t *data = control->readable_data();
                outcome.frame_type = data[3];
                outcome.window_increment = parse_window_update_increment(data);
            }
        }

        promise.set_value(outcome);
        group.stop();
        co_return;
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "window update did not finish in time";
        return;
    }

    const Outcome outcome = future.get();
    group.join();
    ASSERT_TRUE(outcome.stream_created);
    EXPECT_EQ(outcome.result, fiber::common::IoErr::None);
    EXPECT_EQ(outcome.recv_window_remaining, 49);
    EXPECT_EQ(outcome.pending_control_bytes, 13U);
    EXPECT_EQ(outcome.control_bytes, 13U);
    EXPECT_EQ(outcome.frame_type, static_cast<std::uint8_t>(fiber::http::Http2FrameType::WindowUpdate));
    EXPECT_EQ(outcome.window_increment, 34U);
}
