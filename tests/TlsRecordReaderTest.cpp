#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordReader.h>
#include "LoopTestSupport.h"

namespace {

using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;
using fiber::tls::TlsAlertDesc;
using fiber::tls::TlsContentType;
using fiber::tls::TlsRecord;
using fiber::tls::TlsRecordReader;

static_assert(!std::is_copy_constructible_v<TlsRecordReader::Result>);
static_assert(std::is_nothrow_move_constructible_v<TlsRecordReader::Result>);

std::vector<std::uint8_t> make_record(TlsContentType type, std::uint16_t version, std::string_view payload) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(type));
    out.push_back(static_cast<std::uint8_t>(version >> 8));
    out.push_back(static_cast<std::uint8_t>(version & 0xff));
    const std::size_t len = payload.size();
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(len & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

IoBuf make_buf(const std::vector<std::uint8_t> &bytes) {
    IoBuf buf = IoBuf::allocate(bytes.size());
    std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
    buf.commit(bytes.size());
    return buf;
}

IoBuf make_buf(const std::uint8_t *data, std::size_t len) {
    IoBuf buf = IoBuf::allocate(len);
    std::memcpy(buf.writable_data(), data, len);
    buf.commit(len);
    return buf;
}

// Test-side materialization: walks the record's payload chain span by span,
// batch after batch, proving the raw bytes are all there regardless of layout.
std::vector<std::uint8_t> drain_payload(TlsRecord &record) {
    std::vector<std::uint8_t> out;
    out.reserve(record.length);
    struct iovec spans[16];
    while (record.payload.readable_bytes() > 0) {
        const int count = record.payload.fill_write_iov(spans, 16);
        if (count <= 0) {
            ADD_FAILURE() << "fill_write_iov returned " << count << " with readable bytes left";
            break;
        }
        std::size_t copied = 0;
        for (int i = 0; i < count; ++i) {
            const auto *begin = static_cast<const std::uint8_t *>(spans[i].iov_base);
            out.insert(out.end(), begin, begin + spans[i].iov_len);
            copied += spans[i].iov_len;
        }
        record.payload.consume(copied);
    }
    EXPECT_EQ(out.size(), record.length);
    return out;
}

std::string_view contiguous_view_of(const TlsRecord &record) {
    const std::uint8_t *payload = record.contiguous_payload();
    return {reinterpret_cast<const char *>(payload), record.length};
}

TEST(TlsRecordReaderTest, SingleRecordTakenAsOneWholeNode) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        const std::vector<std::uint8_t> bytes = make_record(TlsContentType::Handshake, 0x0303, "hello");
        IoBuf keep;
        {
            IoBuf buf = make_buf(bytes);
            keep = buf; // refcount +1, readable_data() stays stable
            ASSERT_TRUE(reader.feed(std::move(buf)));
        }

        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r.record.type, TlsContentType::Handshake);
        EXPECT_EQ(r.record.legacy_version, 0x0303);
        EXPECT_EQ(r.record.length, 5u);
        EXPECT_EQ(r.record.payload.readable_bytes(), 5u);
        // The fed node moved into the record whole: single span, zero-copy, and
        // the take is immediate (no lazy retirement).
        EXPECT_EQ(r.record.payload.size(), 1u);
        EXPECT_EQ(r.record.contiguous_payload(), keep.readable_data() + fiber::tls::kTlsRecordHeaderSize);
        EXPECT_EQ(contiguous_view_of(r.record), "hello");
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, MultipleRecordsSplitSequentially) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        std::vector<std::uint8_t> bytes = make_record(TlsContentType::Handshake, 0x0301, "abc");
        const std::vector<std::uint8_t> second = make_record(TlsContentType::ApplicationData, 0x0303, "de");
        const std::vector<std::uint8_t> third = make_record(TlsContentType::Alert, 0x0303, "\x01\x28");
        bytes.insert(bytes.end(), second.begin(), second.end());
        bytes.insert(bytes.end(), third.begin(), third.end());
        ASSERT_TRUE(reader.feed(make_buf(bytes)));

        const auto r1 = reader.next();
        ASSERT_EQ(r1.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r1.record.legacy_version, 0x0301);
        EXPECT_NE(r1.record.contiguous_payload(), nullptr);
        EXPECT_EQ(contiguous_view_of(r1.record), "abc");

        const auto r2 = reader.next();
        ASSERT_EQ(r2.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r2.record.type, TlsContentType::ApplicationData);
        EXPECT_EQ(contiguous_view_of(r2.record), "de");

        const auto r3 = reader.next();
        ASSERT_EQ(r3.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r3.record.type, TlsContentType::Alert);
        EXPECT_EQ(r3.record.length, 2u);
        EXPECT_EQ(contiguous_view_of(r3.record), "\x01\x28");

        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, PartialHeaderDefersUntilComplete) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        const std::vector<std::uint8_t> bytes = make_record(TlsContentType::ApplicationData, 0x0303, "xyz");
        ASSERT_TRUE(reader.feed(make_buf(bytes.data(), 3)));
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        EXPECT_EQ(reader.pending_bytes(), 3u);

        ASSERT_TRUE(reader.feed(make_buf(bytes.data() + 3, bytes.size() - 3)));
        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(contiguous_view_of(r.record), "xyz");
    });
}

TEST(TlsRecordReaderTest, RecordAcrossThreeFeedsKeepsSpannedLayout) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        const std::vector<std::uint8_t> bytes = make_record(TlsContentType::ApplicationData, 0x0303, "0123456789");
        ASSERT_TRUE(reader.feed(make_buf(bytes.data(), 6)));
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        ASSERT_TRUE(reader.feed(make_buf(bytes.data() + 6, 2)));
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        ASSERT_TRUE(reader.feed(make_buf(bytes.data() + 8, bytes.size() - 8)));

        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        // Three fed nodes moved into the record as-is: no gather, no copy — the
        // payload is a three-node chain and contiguous_payload() says so.
        EXPECT_EQ(r.record.payload.size(), 3u);
        EXPECT_EQ(r.record.contiguous_payload(), nullptr);
        EXPECT_EQ(drain_payload(r.record),
                  (std::vector<std::uint8_t>{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'}));
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, PayloadStraddlingNodesKeepsSpannedLayout) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        std::string payload(64, 'a');
        payload[0] = '!';
        const std::vector<std::uint8_t> bytes = make_record(TlsContentType::ApplicationData, 0x0303, payload);
        // Header + first 25 payload bytes here, the rest in a second node.
        ASSERT_TRUE(reader.feed(make_buf(bytes.data(), 30)));
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        ASSERT_TRUE(reader.feed(make_buf(bytes.data() + 30, bytes.size() - 30)));

        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r.record.length, 64u);
        EXPECT_EQ(r.record.payload.size(), 2u);
        EXPECT_EQ(r.record.contiguous_payload(), nullptr);
        EXPECT_EQ(drain_payload(r.record), std::vector<std::uint8_t>(payload.begin(), payload.end()));
        EXPECT_EQ(reader.pending_bytes(), 0u);
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
    });
}

TEST(TlsRecordReaderTest, HeaderStraddlingNodesKeepsSpannedLayout) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        const std::vector<std::uint8_t> bytes = make_record(TlsContentType::Handshake, 0x0303, "body");
        ASSERT_TRUE(reader.feed(make_buf(bytes.data(), 3)));
        ASSERT_TRUE(reader.feed(make_buf(bytes.data() + 3, 3)));
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        ASSERT_TRUE(reader.feed(make_buf(bytes.data() + 6, bytes.size() - 6)));

        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r.record.contiguous_payload(), nullptr);
        EXPECT_EQ(drain_payload(r.record), (std::vector<std::uint8_t>{'b', 'o', 'd', 'y'}));
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, OneByteNodesProduceSpannedChain) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        const std::vector<std::uint8_t> bytes =
                make_record(TlsContentType::ApplicationData, 0x0303, "0123456789abcdefghij");
        for (const std::uint8_t byte: bytes) {
            ASSERT_TRUE(reader.feed(make_buf(&byte, 1)));
        }

        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r.record.payload.size(), bytes.size());
        EXPECT_EQ(r.record.contiguous_payload(), nullptr);
        EXPECT_EQ(drain_payload(r.record), std::vector<std::uint8_t>(bytes.begin() + 5, bytes.end()));
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, ZeroLengthRecordPassesThrough) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        std::vector<std::uint8_t> bytes = make_record(TlsContentType::ApplicationData, 0x0303, "");
        const std::vector<std::uint8_t> second = make_record(TlsContentType::ApplicationData, 0x0303, "next");
        bytes.insert(bytes.end(), second.begin(), second.end());
        ASSERT_TRUE(reader.feed(make_buf(bytes)));

        const auto r1 = reader.next();
        ASSERT_EQ(r1.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r1.record.length, 0u);
        EXPECT_EQ(r1.record.payload.readable_bytes(), 0u);
        // Empty records have no payload span by definition.
        EXPECT_EQ(r1.record.contiguous_payload(), nullptr);

        const auto r2 = reader.next();
        ASSERT_EQ(r2.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(contiguous_view_of(r2.record), "next");
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, UnknownContentTypeIsFatal) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        // heartbeat(24) and 0x00 are not valid TLS content types.
        for (const std::uint8_t type: {std::uint8_t{24}, std::uint8_t{0}}) {

            TlsRecordReader reader;
            const std::vector<std::uint8_t> bytes = make_record(static_cast<TlsContentType>(type), 0x0303, "\x01");
            ASSERT_TRUE(reader.feed(make_buf(bytes)));

            auto r = reader.next();
            EXPECT_EQ(r.status, TlsRecordReader::Result::Status::Fatal);
            EXPECT_EQ(r.alert, TlsAlertDesc::UnexpectedMessage);
        }
    });
}

TEST(TlsRecordReaderTest, OversizedLengthIsFatalRecordOverflow) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        const std::uint16_t oversize = static_cast<std::uint16_t>(fiber::tls::kTlsMaxCiphertextRecordSize + 1);
        const std::array<std::uint8_t, 5> header{23, 0x03, 0x03, static_cast<std::uint8_t>(oversize >> 8),
                                                 static_cast<std::uint8_t>(oversize & 0xff)};
        ASSERT_TRUE(reader.feed(make_buf(header.data(), header.size())));

        auto r = reader.next();
        EXPECT_EQ(r.status, TlsRecordReader::Result::Status::Fatal);
        EXPECT_EQ(r.alert, TlsAlertDesc::RecordOverflow);
    });
}

TEST(TlsRecordReaderTest, MaxCiphertextLengthFramingOk) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        std::vector<std::uint8_t> bytes;
        bytes.push_back(23);
        bytes.push_back(0x03);
        bytes.push_back(0x03);
        const std::size_t len = fiber::tls::kTlsMaxCiphertextRecordSize;
        bytes.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
        bytes.push_back(static_cast<std::uint8_t>(len & 0xff));
        bytes.resize(fiber::tls::kTlsRecordHeaderSize + len, 0x5a);
        ASSERT_TRUE(reader.feed(make_buf(bytes)));

        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r.record.length, len);
        EXPECT_NE(r.record.contiguous_payload(), nullptr);
        EXPECT_EQ(r.record.contiguous_payload()[0], 0x5a);
        EXPECT_EQ(r.record.contiguous_payload()[len - 1], 0x5a);
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

TEST(TlsRecordReaderTest, TakenRecordOutlivesReader) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        auto taken = std::optional<TlsRecord>{};
        {
            TlsRecordReader reader;
            ASSERT_TRUE(reader.feed(make_buf(make_record(TlsContentType::ApplicationData, 0x0303, "owned"))));
            auto r = reader.next();
            ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
            taken.emplace(std::move(r.record));
        }

        ASSERT_TRUE(taken.has_value());
        EXPECT_EQ(contiguous_view_of(*taken), "owned");
        taken.reset(); // release the chain nodes back to the loop's pool
    });
}

TEST(TlsRecordReaderTest, ResetClearsPendingOnly) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        ASSERT_TRUE(reader.feed(make_buf(make_record(TlsContentType::ApplicationData, 0x0303, "keep"))));
        auto r = reader.next();
        ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
        const std::string_view snapshot = contiguous_view_of(r.record);

        reader.reset();
        EXPECT_EQ(reader.pending_bytes(), 0u);
        EXPECT_EQ(reader.next().status, TlsRecordReader::Result::Status::NeedMore);
        // The taken record is independent of the reader's pending buffer.
        EXPECT_EQ(snapshot, "keep");

        ASSERT_TRUE(reader.feed(make_buf(make_record(TlsContentType::ApplicationData, 0x0303, "after"))));
        const auto r2 = reader.next();
        ASSERT_EQ(r2.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(contiguous_view_of(r2.record), "after");
    });
}

TEST(TlsRecordReaderTest, LegacyVersionsParsedNotRejected) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        for (const std::uint16_t version: {0x0301, 0x0302, 0x0303}) {
            ASSERT_TRUE(reader.feed(make_buf(make_record(TlsContentType::Handshake, version, "v"))));
            auto r = reader.next();
            ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
            EXPECT_EQ(r.record.legacy_version, version);
        }
    });
}

TEST(TlsRecordReaderTest, ContiguousAndSpannedRecordsInterleave) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &pool) {
        TlsRecordReader reader;

        // One buffer holding a complete record plus the head of the next one.
        const std::vector<std::uint8_t> first = make_record(TlsContentType::Handshake, 0x0303, "complete");
        const std::vector<std::uint8_t> second =
                make_record(TlsContentType::ApplicationData, 0x0303, "straddled-payload");
        std::vector<std::uint8_t> bytes = first;
        bytes.insert(bytes.end(), second.begin(), second.begin() + 10);
        ASSERT_TRUE(reader.feed(make_buf(bytes)));

        const auto r1 = reader.next();
        ASSERT_EQ(r1.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r1.record.payload.size(), 1u);
        EXPECT_EQ(contiguous_view_of(r1.record), "complete");
        EXPECT_EQ(reader.pending_bytes(), bytes.size() - first.size());

        ASSERT_TRUE(reader.feed(make_buf(second.data() + 10, second.size() - 10)));
        auto r2 = reader.next();
        ASSERT_EQ(r2.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r2.record.payload.size(), 2u);
        EXPECT_EQ(r2.record.contiguous_payload(), nullptr);
        EXPECT_EQ(drain_payload(r2.record), std::vector<std::uint8_t>(second.begin() + 5, second.end()));
        EXPECT_EQ(reader.pending_bytes(), 0u);

        // Back to a single-span record aligned in its own node.
        ASSERT_TRUE(reader.feed(make_buf(make_record(TlsContentType::Alert, 0x0303, "\x02\x28"))));
        const auto r3 = reader.next();
        ASSERT_EQ(r3.status, TlsRecordReader::Result::Status::Ok);
        EXPECT_EQ(r3.record.type, TlsContentType::Alert);
        EXPECT_EQ(contiguous_view_of(r3.record), "\x02\x28");
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

} // namespace
