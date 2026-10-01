#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordFramer.h>
#include <fiber/tls/record/TlsRecordReader.h>
#include "LoopTestSupport.h"

namespace {

using fiber::mem::IoBuf;
using fiber::tls::kTlsMaxCiphertextRecordSize;
using fiber::tls::kTlsRecordBatchMax;
using fiber::tls::kTlsRecordHeaderSize;
using fiber::tls::tls_frame_records;
using fiber::tls::TlsAlertDesc;
using fiber::tls::TlsContentType;
using fiber::tls::TlsFrameResult;
using fiber::tls::TlsRecordReader;
using fiber::tls::TlsRecordSpan;

static_assert(std::is_trivially_copyable_v<TlsRecordSpan>);
static_assert(sizeof(TlsRecordSpan) <= 12);

using Batch = std::array<TlsRecordSpan, kTlsRecordBatchMax>;

// Appends one record whose payload is `len` bytes of a seed-derived pattern.
void append_record(std::vector<std::uint8_t> &wire, TlsContentType type, std::uint16_t version, std::size_t len,
                   std::uint8_t seed = 0) {
    wire.push_back(static_cast<std::uint8_t>(type));
    wire.push_back(static_cast<std::uint8_t>(version >> 8));
    wire.push_back(static_cast<std::uint8_t>(version & 0xff));
    wire.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
    wire.push_back(static_cast<std::uint8_t>(len & 0xff));
    for (std::size_t i = 0; i < len; ++i) {
        wire.push_back(static_cast<std::uint8_t>(seed + i * 31));
    }
}

std::span<const std::uint8_t> prefix(const std::vector<std::uint8_t> &wire, std::size_t len) {
    return {wire.data(), len};
}

void expect_nothing_framed(const TlsFrameResult &result) {
    EXPECT_EQ(result.count, 0u);
    EXPECT_EQ(result.consumed, 0u);
    EXPECT_FALSE(result.fatal);
}

TEST(TlsRecordFramerTest, EmptyAndPartialHeaderFrameNothing) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 3);
    Batch batch{};
    for (std::size_t len = 0; len < kTlsRecordHeaderSize; ++len) {
        expect_nothing_framed(tls_frame_records(prefix(wire, len), batch));
    }
}

TEST(TlsRecordFramerTest, IncompleteBodyFramesNothing) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 100);
    Batch batch{};
    for (std::size_t len = kTlsRecordHeaderSize; len < wire.size(); ++len) {
        expect_nothing_framed(tls_frame_records(prefix(wire, len), batch));
    }
}

TEST(TlsRecordFramerTest, SingleRecordFramesExactly) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::Handshake, 0x0301, 7);
    Batch batch{};
    const TlsFrameResult result = tls_frame_records(wire, batch);
    ASSERT_EQ(result.count, 1u);
    EXPECT_EQ(result.consumed, wire.size());
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(batch[0].type, TlsContentType::Handshake);
    EXPECT_EQ(batch[0].legacy_version, 0x0301);
    EXPECT_EQ(batch[0].length, 7u);
    EXPECT_EQ(batch[0].offset, kTlsRecordHeaderSize);
}

TEST(TlsRecordFramerTest, CompleteRecordsThenIncompleteTail) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::Handshake, 0x0303, 3, 1);
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 200, 2);
    append_record(wire, TlsContentType::Alert, 0x0303, 2, 3);
    const std::size_t complete = wire.size();
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 50, 4);
    wire.resize(wire.size() - 1); // the last record misses one body byte

    Batch batch{};
    const TlsFrameResult result = tls_frame_records(wire, batch);
    ASSERT_EQ(result.count, 3u);
    EXPECT_EQ(result.consumed, complete);
    EXPECT_FALSE(result.fatal);

    const std::size_t lengths[] = {3, 200, 2};
    const TlsContentType types[] = {TlsContentType::Handshake, TlsContentType::ApplicationData, TlsContentType::Alert};
    std::size_t off = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(batch[i].type, types[i]);
        EXPECT_EQ(batch[i].length, lengths[i]);
        EXPECT_EQ(batch[i].offset, off + kTlsRecordHeaderSize);
        // The offset addresses the payload itself: its first byte is the
        // pattern's seed.
        EXPECT_EQ(wire[batch[i].offset], static_cast<std::uint8_t>(i + 1));
        off += kTlsRecordHeaderSize + lengths[i];
    }
}

TEST(TlsRecordFramerTest, StopsWhenTheBatchIsFull) {
    std::vector<std::uint8_t> wire;
    for (std::size_t i = 0; i < kTlsRecordBatchMax + 1; ++i) {
        append_record(wire, TlsContentType::ApplicationData, 0x0303, i, static_cast<std::uint8_t>(i));
    }
    Batch batch{};
    const TlsFrameResult first = tls_frame_records(wire, batch);
    ASSERT_EQ(first.count, kTlsRecordBatchMax);
    EXPECT_FALSE(first.fatal);
    EXPECT_EQ(batch[kTlsRecordBatchMax - 1].length, kTlsRecordBatchMax - 1);

    // The remainder frames on its own; offsets restart at the new region.
    const TlsFrameResult second = tls_frame_records(std::span<const std::uint8_t>(wire).subspan(first.consumed), batch);
    ASSERT_EQ(second.count, 1u);
    EXPECT_EQ(first.consumed + second.consumed, wire.size());
    EXPECT_EQ(batch[0].offset, kTlsRecordHeaderSize);
    EXPECT_EQ(batch[0].length, kTlsRecordBatchMax);
}

TEST(TlsRecordFramerTest, ShorterOutputSpanBoundsTheBatch) {
    std::vector<std::uint8_t> wire;
    for (std::size_t i = 0; i < 4; ++i) {
        append_record(wire, TlsContentType::ApplicationData, 0x0303, 10);
    }
    std::array<TlsRecordSpan, 2> two{};
    const TlsFrameResult result = tls_frame_records(wire, two);
    EXPECT_EQ(result.count, 2u);
    EXPECT_EQ(result.consumed, 2 * (kTlsRecordHeaderSize + 10));

    expect_nothing_framed(tls_frame_records(wire, std::span<TlsRecordSpan>{}));
}

TEST(TlsRecordFramerTest, UnknownContentTypeIsFatalAfterTheRecordsBeforeIt) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 4);
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 6);
    const std::size_t good = wire.size();
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 1);
    wire[good] = 0x18; // heartbeat: not a content type we accept

    Batch batch{};
    const TlsFrameResult result = tls_frame_records(wire, batch);
    EXPECT_EQ(result.count, 2u);
    EXPECT_EQ(result.consumed, good);
    EXPECT_TRUE(result.fatal);
    EXPECT_EQ(result.alert, TlsAlertDesc::UnexpectedMessage);

    // First record bad: nothing framed, still fatal.
    const TlsFrameResult head = tls_frame_records(std::span<const std::uint8_t>(wire).subspan(good), batch);
    EXPECT_EQ(head.count, 0u);
    EXPECT_EQ(head.consumed, 0u);
    EXPECT_TRUE(head.fatal);
    EXPECT_EQ(head.alert, TlsAlertDesc::UnexpectedMessage);
}

TEST(TlsRecordFramerTest, BadTypeNeedsTheWholeHeader) {
    // Like the chain reader, the type is judged once the header is complete.
    const std::uint8_t bad[] = {0x18, 0x03, 0x03, 0x00};
    Batch batch{};
    expect_nothing_framed(tls_frame_records(bad, batch));
}

TEST(TlsRecordFramerTest, OversizeLengthIsFatalBeforeTheBody) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 0);
    const std::size_t len = kTlsMaxCiphertextRecordSize + 1;
    wire[3] = static_cast<std::uint8_t>(len >> 8);
    wire[4] = static_cast<std::uint8_t>(len & 0xff);

    Batch batch{};
    const TlsFrameResult result = tls_frame_records(wire, batch);
    EXPECT_EQ(result.count, 0u);
    EXPECT_TRUE(result.fatal);
    EXPECT_EQ(result.alert, TlsAlertDesc::RecordOverflow);
}

TEST(TlsRecordFramerTest, MaxLengthWaitsForItsBodyThenFrames) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ApplicationData, 0x0303, kTlsMaxCiphertextRecordSize);
    Batch batch{};
    expect_nothing_framed(tls_frame_records(prefix(wire, wire.size() - 1), batch));

    const TlsFrameResult result = tls_frame_records(wire, batch);
    ASSERT_EQ(result.count, 1u);
    EXPECT_EQ(result.consumed, wire.size());
    EXPECT_EQ(batch[0].length, kTlsMaxCiphertextRecordSize);
}

TEST(TlsRecordFramerTest, ZeroLengthRecordsPassThrough) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ApplicationData, 0x0303, 0);
    append_record(wire, TlsContentType::Handshake, 0x0303, 0);
    Batch batch{};
    const TlsFrameResult result = tls_frame_records(wire, batch);
    ASSERT_EQ(result.count, 2u);
    EXPECT_EQ(result.consumed, 2 * kTlsRecordHeaderSize);
    EXPECT_EQ(batch[0].offset, kTlsRecordHeaderSize);
    EXPECT_EQ(batch[0].length, 0u);
    EXPECT_EQ(batch[1].offset, 2 * kTlsRecordHeaderSize);
    EXPECT_EQ(batch[1].type, TlsContentType::Handshake);
}

TEST(TlsRecordFramerTest, EveryContentTypeAndLegacyVersionAccepted) {
    std::vector<std::uint8_t> wire;
    append_record(wire, TlsContentType::ChangeCipherSpec, 0x0300, 1);
    append_record(wire, TlsContentType::Alert, 0x0301, 2);
    append_record(wire, TlsContentType::Handshake, 0x0302, 3);
    append_record(wire, TlsContentType::ApplicationData, 0xffff, 4);
    Batch batch{};
    const TlsFrameResult result = tls_frame_records(wire, batch);
    ASSERT_EQ(result.count, 4u);
    EXPECT_FALSE(result.fatal);
    EXPECT_EQ(batch[0].type, TlsContentType::ChangeCipherSpec);
    EXPECT_EQ(batch[0].legacy_version, 0x0300);
    EXPECT_EQ(batch[3].type, TlsContentType::ApplicationData);
    EXPECT_EQ(batch[3].legacy_version, 0xffff);
}

// Parity with the chain reader the handshake keeps using: the same bytes
// yield the same records, the same stop point and the same alert.
TEST(TlsRecordFramerTest, MatchesTheChainReader) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto check = [](const std::vector<std::uint8_t> &wire) {
            TlsRecordReader reader;
            IoBuf buf = IoBuf::allocate(wire.size());
            std::memcpy(buf.writable_data(), wire.data(), wire.size());
            buf.commit(wire.size());
            ASSERT_TRUE(reader.feed(std::move(buf)));

            std::span<const std::uint8_t> rest(wire);
            Batch batch{};
            for (;;) {
                const TlsFrameResult framed = tls_frame_records(rest, batch);
                for (std::size_t i = 0; i < framed.count; ++i) {
                    TlsRecordReader::Result next = reader.next();
                    ASSERT_EQ(next.status, TlsRecordReader::Result::Status::Ok);
                    EXPECT_EQ(next.record.type, batch[i].type);
                    EXPECT_EQ(next.record.legacy_version, batch[i].legacy_version);
                    ASSERT_EQ(next.record.length, batch[i].length);
                    if (next.record.length > 0) {
                        EXPECT_EQ(0, std::memcmp(next.record.contiguous_payload(), rest.data() + batch[i].offset,
                                                 batch[i].length));
                    }
                }
                rest = rest.subspan(framed.consumed);
                if (framed.fatal) {
                    TlsRecordReader::Result next = reader.next();
                    ASSERT_EQ(next.status, TlsRecordReader::Result::Status::Fatal);
                    EXPECT_EQ(next.alert, framed.alert);
                    return;
                }
                if (framed.count < batch.size()) {
                    EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
                    EXPECT_EQ(reader.pending_bytes(), rest.size());
                    return;
                }
            }
        };

        std::vector<std::uint8_t> wire;
        for (std::size_t i = 0; i < 70; ++i) {
            const auto type = static_cast<TlsContentType>(20 + i % 4);
            append_record(wire, type, static_cast<std::uint16_t>(0x0301 + i % 3), (i * 37) % 300,
                          static_cast<std::uint8_t>(i));
        }
        check(wire); // three batches, ends on a record boundary

        std::vector<std::uint8_t> tail = wire;
        tail.resize(tail.size() - 3); // incomplete last record
        check(tail);

        std::vector<std::uint8_t> bad_type = wire;
        append_record(bad_type, TlsContentType::ApplicationData, 0x0303, 1);
        bad_type[wire.size()] = 0x30;
        check(bad_type);

        std::vector<std::uint8_t> oversize = wire;
        append_record(oversize, TlsContentType::ApplicationData, 0x0303, 0);
        oversize[wire.size() + 3] = 0xff;
        oversize[wire.size() + 4] = 0xff;
        check(oversize);
    });
}

} // namespace
