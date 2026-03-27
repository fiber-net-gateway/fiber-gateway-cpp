#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "async/Spawn.h"
#include "event/EventLoopGroup.h"
#include "http/HttpHeaderHash.h"
#include "http/Http2HeadersFrameEncoder.h"
#include "http/Http2HpackEncodeCatalog.h"
#include "http/Http2HpackDecoder.h"
#include "http/Http2HpackEncoder.h"
#include "http/Http2OutboundScheduler.h"
#include "http/HttpTransport.h"
#include "http/Http2Stream.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http2HeadersFrameEncoder;
using fiber::http::Http2HpackEncodeCatalog;
using fiber::http::Http2HpackEncoder;

class RecordingTransport final : public fiber::http::HttpTransport {
public:
    fiber::async::Task<fiber::common::IoResult<void>> handshake(std::chrono::milliseconds) override {
        co_return fiber::common::IoResult<void>{};
    }

    fiber::async::Task<fiber::common::IoResult<void>> shutdown(std::chrono::milliseconds) override {
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

    fiber::async::Task<fiber::common::IoResult<size_t>> write(const void *buf, size_t len,
                                                              std::chrono::milliseconds) override {
        if (closed_) {
            co_return std::unexpected(fiber::common::IoErr::ConnReset);
        }
        const auto *ptr = static_cast<const std::uint8_t *>(buf);
        written_.insert(written_.end(), ptr, ptr + len);
        co_return len;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> write(fiber::mem::IoBuf &buf,
                                                              std::chrono::milliseconds timeout) override {
        auto result = co_await write(buf.readable_data(), buf.readable(), timeout);
        if (result) {
            buf.consume(*result);
        }
        co_return result;
    }

    fiber::async::Task<fiber::common::IoResult<size_t>> writev(fiber::mem::IoBufChain &buf,
                                                               std::chrono::milliseconds timeout) override {
        auto *front = buf.first_readable();
        if (!front) {
            co_return static_cast<size_t>(0);
        }
        auto result = co_await write(front->readable_data(), front->readable(), timeout);
        if (result) {
            buf.consume_and_compact(*result);
        }
        co_return result;
    }

    void close() override { closed_ = true; }
    [[nodiscard]] bool valid() const noexcept override { return !closed_; }
    [[nodiscard]] int fd() const noexcept override { return -1; }
    [[nodiscard]] std::string negotiated_alpn() const noexcept override { return "h2"; }
    [[nodiscard]] const fiber::net::SocketAddress &remote_addr() const noexcept override { return remote_addr_; }
    [[nodiscard]] fiber::event::EventLoop &loop() const noexcept override { return loop_ ? *loop_ : fallback_loop_; }
    [[nodiscard]] const std::vector<std::uint8_t> &written() const noexcept { return written_; }

private:
    bool closed_ = false;
    std::vector<std::uint8_t> written_;
    fiber::net::SocketAddress remote_addr_{};
    fiber::event::EventLoop *loop_ = fiber::event::EventLoop::current_or_null();
    mutable fiber::event::EventLoop fallback_loop_{};
};

struct EncodeCase {
    int status_code = 0;
    Http2HeadersFrameEncoder::Options options{};
    std::vector<std::pair<std::string_view, std::string_view>> headers;
    std::size_t slot_used = 0;
    std::size_t total_bytes = 0;
};

fiber::common::IoErr on_header_block_start(void *, fiber::http::Http2HpackDecoder::Sink &) noexcept {
    return fiber::common::IoErr::None;
}

fiber::common::IoErr on_header_block_complete(void *, bool) noexcept {
    return fiber::common::IoErr::None;
}

fiber::common::IoErr on_body(void *, fiber::mem::IoBuf &&, bool) noexcept {
    return fiber::common::IoErr::None;
}

void on_abort(void *, fiber::common::IoErr) noexcept {}
void on_destroy(void *) noexcept {}

const fiber::http::Http2Stream::Ops kStreamOps{
    &on_destroy,
    &on_header_block_start,
    &on_header_block_complete,
    &on_body,
    &on_abort,
    nullptr,
};

fiber::common::IoErr encode_headers_to_target(fiber::http::Http2Stream &, void *ctx,
                                              const fiber::http::Http2OutboundEncodeRequest &,
                                              fiber::http::Http2OutboundEncodeTarget &target,
                                              fiber::http::Http2OutboundEncodeResult &result) noexcept {
    auto *test_case = static_cast<EncodeCase *>(ctx);
    if (!test_case) {
        return fiber::common::IoErr::Invalid;
    }

    Http2HpackEncodeCatalog catalog;
    if (!catalog.init({})) {
        return fiber::common::IoErr::Invalid;
    }

    Http2HpackEncoder encoder({.catalog = &catalog, .huffman_threshold = 1024});
    if (!encoder.init()) {
        return fiber::common::IoErr::Invalid;
    }

    Http2HeadersFrameEncoder frame_encoder(encoder, test_case->options);
    fiber::common::IoErr err = frame_encoder.begin(target);
    if (err != fiber::common::IoErr::None) {
        return err;
    }
    err = frame_encoder.encode_status(test_case->status_code);
    if (err != fiber::common::IoErr::None) {
        frame_encoder.abort();
        return err;
    }
    for (const auto &[name, value] : test_case->headers) {
        err = frame_encoder.encode_field(name, fiber::http::http_header_name_hash(name), value);
        if (err != fiber::common::IoErr::None) {
            frame_encoder.abort();
            return err;
        }
    }
    err = frame_encoder.finish();
    if (err != fiber::common::IoErr::None) {
        return err;
    }

    test_case->slot_used = target.slot_used();
    test_case->total_bytes = target.total_bytes();

    result.status = fiber::http::Http2OutboundEncodeResult::Status::Encoded;
    result.next_kind = fiber::http::Http2OutboundNextKind::None;
    result.consumed_conn_window = 0;
    return fiber::common::IoErr::None;
}

std::vector<std::uint8_t> encode_headers_bytes_in_place(EncodeCase &test_case) {
    fiber::event::EventLoopGroup group(1);
    auto promise = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
    auto future = promise->get_future();

    group.start();
    fiber::async::spawn(group.at(0), [promise, &test_case, &group]() mutable
        -> fiber::async::DetachedTask {
        RecordingTransport transport;
        fiber::http::Http2OutboundScheduler scheduler(&transport, 1024, std::chrono::seconds(30),
                                                      test_case.options.max_frame_size);
        int owner = 0;
        fiber::http::Http2Stream stream(test_case.options.stream_id, &owner, kStreamOps);

        fiber::common::IoErr err = scheduler.request_send(stream, fiber::http::Http2OutboundNextKind::Headers,
                                                          &encode_headers_to_target, &test_case);
        if (err == fiber::common::IoErr::None) {
            scheduler.close();
            co_await scheduler.send_loop();
            err = scheduler.stop_reason();
        }

        if (err != fiber::common::IoErr::None) {
            promise->set_value({});
        } else {
            promise->set_value(transport.written());
        }
        group.stop();
        co_return;
    });

    EXPECT_EQ(future.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    std::vector<std::uint8_t> result = future.get();
    group.join();
    return result;
}

std::vector<std::uint8_t> encode_headers_bytes(EncodeCase test_case) {
    return encode_headers_bytes_in_place(test_case);
}

TEST(Http2HeadersFrameEncoderTest, EncodesSingleHeadersFrame) {
    EXPECT_EQ(encode_headers_bytes({
                  .status_code = 200,
                  .options =
                      {
                          .stream_id = 1,
                          .max_frame_size = 16384,
                          .first_frame_payload_cap = 1024,
                      },
              }),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01, 0x88}));
}

TEST(Http2HeadersFrameEncoderTest, SplitsHeaderBlockIntoContinuationFrames) {
    EXPECT_EQ(encode_headers_bytes({
                  .status_code = 418,
                  .options =
                      {
                          .stream_id = 3,
                          .max_frame_size = 4,
                          .first_frame_payload_cap = 4,
                      },
              }),
              (std::vector<std::uint8_t>{
                  0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x08, 0x03, '4', '1',
                  0x00, 0x00, 0x01, 0x09, 0x04, 0x00, 0x00, 0x00, 0x03, '8',
              }));
}

TEST(Http2HeadersFrameEncoderTest, KeepsSingleFrameAcrossMultipleIoBufs) {
    std::vector<std::uint8_t> out = encode_headers_bytes({
        .status_code = 418,
        .options =
            {
                .stream_id = 7,
                .max_frame_size = 16,
                .first_frame_payload_cap = 4,
            },
    });
    EXPECT_EQ(out.size(), 14U);
    EXPECT_EQ(out,
              (std::vector<std::uint8_t>{
                  0x00, 0x00, 0x05, 0x01, 0x04, 0x00, 0x00, 0x00, 0x07, 0x08, 0x03, '4', '1', '8',
              }));
}

TEST(Http2HeadersFrameEncoderTest, EncodesHeadersFrameWithPaddingAndPriority) {
    EXPECT_EQ(encode_headers_bytes({
                  .status_code = 200,
                  .options =
                      {
                          .stream_id = 5,
                          .max_frame_size = 32,
                          .first_frame_payload_cap = 32,
                          .end_stream = true,
                          .pad_length = 2,
                          .has_priority = true,
                          .exclusive = true,
                          .stream_dependency = 3,
                          .weight = 10,
                      },
              }),
              (std::vector<std::uint8_t>{
                  0x00, 0x00, 0x09, 0x01, 0x2d, 0x00, 0x00, 0x00, 0x05,
                  0x02, 0x80, 0x00, 0x00, 0x03, 0x0a, 0x88, 0x00, 0x00,
              }));
}

TEST(Http2HeadersFrameEncoderTest, SmallHeaderBlockStaysInTargetSlot) {
    EncodeCase test_case{
        .status_code = 200,
        .options =
            {
                .stream_id = 9,
                .max_frame_size = 16384,
                .first_frame_payload_cap = 16384,
            },
    };

    EXPECT_EQ(encode_headers_bytes_in_place(test_case),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x01, 0x04, 0x00, 0x00, 0x00, 0x09, 0x88}));
    EXPECT_NE(test_case.slot_used, 0U);
    EXPECT_EQ(test_case.slot_used, test_case.total_bytes);
}

} // namespace
