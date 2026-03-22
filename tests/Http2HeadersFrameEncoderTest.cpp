#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "common/mem/IoBuf.h"
#include "http/Http2HeadersFrameEncoder.h"
#include "http/Http2HpackEncodeCatalog.h"
#include "http/Http2HpackEncoder.h"

namespace {

using fiber::common::IoErr;
using fiber::http::Http2HeadersFrameEncoder;
using fiber::http::Http2HpackEncodeCatalog;
using fiber::http::Http2HpackEncoder;
using fiber::mem::IoBufChain;

std::vector<std::uint8_t> chain_to_bytes(IoBufChain chain) {
    std::vector<std::uint8_t> out;
    out.reserve(chain.readable_bytes());
    while (auto *front = chain.front()) {
        if (front->readable() == 0) {
            chain.drop_empty_front();
            continue;
        }
        const std::uint8_t *data = front->readable_data();
        out.insert(out.end(), data, data + front->readable());
        chain.consume_and_compact(front->readable());
    }
    return out;
}

TEST(Http2HeadersFrameEncoderTest, EncodesSingleHeadersFrame) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog});
    ASSERT_TRUE(encoder.init());

    Http2HeadersFrameEncoder frame_encoder(encoder, {
                                                        .stream_id = 1,
                                                        .max_frame_size = 16384,
                                                        .first_frame_payload_cap = 1024,
                                                    });
    ASSERT_EQ(frame_encoder.begin(), IoErr::None);
    ASSERT_EQ(frame_encoder.encode_status(200), IoErr::None);

    IoBufChain out;
    ASSERT_EQ(frame_encoder.finish(out), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(out)),
              (std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x01, 0x04, 0x00, 0x00, 0x00, 0x01, 0x88}));
}

TEST(Http2HeadersFrameEncoderTest, SplitsHeaderBlockIntoContinuationFrames) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog, .huffman_threshold = 1024});
    ASSERT_TRUE(encoder.init());

    Http2HeadersFrameEncoder frame_encoder(encoder, {
                                                        .stream_id = 3,
                                                        .max_frame_size = 4,
                                                        .first_frame_payload_cap = 4,
                                                    });
    ASSERT_EQ(frame_encoder.begin(), IoErr::None);
    ASSERT_EQ(frame_encoder.encode_status(418), IoErr::None);

    IoBufChain out;
    ASSERT_EQ(frame_encoder.finish(out), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(out)),
              (std::vector<std::uint8_t>{
                  0x00, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 0x08, 0x03, '4', '1',
                  0x00, 0x00, 0x01, 0x09, 0x04, 0x00, 0x00, 0x00, 0x03, '8',
              }));
}

TEST(Http2HeadersFrameEncoderTest, KeepsSingleFrameAcrossMultipleIoBufs) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog, .huffman_threshold = 1024});
    ASSERT_TRUE(encoder.init());

    Http2HeadersFrameEncoder frame_encoder(encoder, {
                                                        .stream_id = 7,
                                                        .max_frame_size = 16,
                                                        .first_frame_payload_cap = 4,
                                                    });
    ASSERT_EQ(frame_encoder.begin(), IoErr::None);
    ASSERT_EQ(frame_encoder.encode_status(418), IoErr::None);

    IoBufChain out;
    ASSERT_EQ(frame_encoder.finish(out), IoErr::None);
    EXPECT_GT(out.size(), 1U);
    EXPECT_EQ(chain_to_bytes(std::move(out)),
              (std::vector<std::uint8_t>{
                  0x00, 0x00, 0x05, 0x01, 0x04, 0x00, 0x00, 0x00, 0x07, 0x08, 0x03, '4', '1', '8',
              }));
}

TEST(Http2HeadersFrameEncoderTest, EncodesHeadersFrameWithPaddingAndPriority) {
    Http2HpackEncodeCatalog catalog;
    ASSERT_TRUE(catalog.init({}));

    Http2HpackEncoder encoder({.catalog = &catalog});
    ASSERT_TRUE(encoder.init());

    Http2HeadersFrameEncoder frame_encoder(encoder, {
                                                        .stream_id = 5,
                                                        .max_frame_size = 32,
                                                        .first_frame_payload_cap = 32,
                                                        .end_stream = true,
                                                        .pad_length = 2,
                                                        .has_priority = true,
                                                        .exclusive = true,
                                                        .stream_dependency = 3,
                                                        .weight = 10,
                                                    });
    ASSERT_EQ(frame_encoder.begin(), IoErr::None);
    ASSERT_EQ(frame_encoder.encode_status(200), IoErr::None);

    IoBufChain out;
    ASSERT_EQ(frame_encoder.finish(out), IoErr::None);
    EXPECT_EQ(chain_to_bytes(std::move(out)),
              (std::vector<std::uint8_t>{
                  0x00, 0x00, 0x09, 0x01, 0x2d, 0x00, 0x00, 0x00, 0x05,
                  0x02, 0x80, 0x00, 0x00, 0x03, 0x0a, 0x88, 0x00, 0x00,
              }));
}

} // namespace
