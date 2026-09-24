#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <fiber/common/mem/IoBuf.h>
#include <fiber/common/mem/IoBufChain.h>
#include <fiber/tls/record/TlsRecord.h>
#include <fiber/tls/record/TlsRecordReader.h>
#include <fiber/tls/record/TlsRecordWriter.h>
#include "LoopTestSupport.h"

namespace {

using fiber::mem::IoBuf;
using fiber::mem::IoBufChain;
using fiber::mem::IoBufNodePool;
using fiber::tls::tls_decode_record_header;
using fiber::tls::tls_encode_record_header;
using fiber::tls::TlsContentType;
using fiber::tls::TlsRecordReader;
using fiber::tls::TlsRecordWriter;

constexpr std::size_t kMaxPlain = fiber::tls::kTlsMaxPlaintextSize;

std::vector<std::uint8_t> ramp(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> out(len);
    for (std::size_t i = 0; i < len; ++i) {
        out[i] = static_cast<std::uint8_t>(seed + i);
    }
    return out;
}

IoBuf make_buf(const std::vector<std::uint8_t> &bytes) {
    IoBuf buf = IoBuf::allocate(bytes.size());
    std::memcpy(buf.writable_data(), bytes.data(), bytes.size());
    buf.commit(bytes.size());
    return buf;
}

// Materializes a chain's readable bytes, batch after batch.
void drain(IoBufChain &chain, std::vector<std::uint8_t> &out) {
    struct iovec spans[16];
    while (chain.readable_bytes() > 0) {
        const int count = chain.fill_write_iov(spans, 16);
        ASSERT_GT(count, 0);
        std::size_t copied = 0;
        for (int i = 0; i < count; ++i) {
            const auto *begin = static_cast<const std::uint8_t *>(spans[i].iov_base);
            out.insert(out.end(), begin, begin + spans[i].iov_len);
            copied += spans[i].iov_len;
        }
        chain.consume(copied);
    }
}

struct ParsedRecord {
    TlsContentType type;
    std::uint16_t version;
    std::vector<std::uint8_t> payload;
};

// Scans framed bytes back into records.
void parse_records(const std::vector<std::uint8_t> &framed, std::vector<ParsedRecord> &out) {
    std::size_t pos = 0;
    while (pos < framed.size()) {
        ASSERT_GE(framed.size() - pos, fiber::tls::kTlsRecordHeaderSize);
        const auto header = tls_decode_record_header(framed.data() + pos);
        ASSERT_TRUE(header.has_value());
        const std::size_t body = header->length;
        ASSERT_GE(framed.size() - pos - fiber::tls::kTlsRecordHeaderSize, body);
        out.push_back(
                ParsedRecord{header->type, header->legacy_version,
                             std::vector<std::uint8_t>(framed.begin() + pos + 5, framed.begin() + pos + 5 + body)});
        pos += 5 + body;
    }
}

std::vector<std::uint8_t> concat_bytes(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto &part: parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

TEST(TlsRecordHeaderCodec, EncodeDecodeRoundTrip) {
    for (const auto type: {TlsContentType::ChangeCipherSpec, TlsContentType::Alert, TlsContentType::Handshake,
                           TlsContentType::ApplicationData}) {
        for (const std::uint16_t version: {0x0300, 0x0301, 0x0303}) {
            for (const std::uint16_t length: {0, 1, 0x4000, 0x4000 + 2048}) {
                std::uint8_t wire[5];
                tls_encode_record_header(wire, type, version, length);
                const auto decoded = tls_decode_record_header(wire);
                ASSERT_TRUE(decoded.has_value());
                EXPECT_EQ(decoded->type, type);
                EXPECT_EQ(decoded->legacy_version, version);
                EXPECT_EQ(decoded->length, length);
            }
        }
    }
}

class TlsRecordWriterTest : public ::testing::Test {
protected:
    TlsRecordWriter writer;
    IoBufChain out;

    // Chains must empty while a loop is current: the fixture is destroyed on
    // the plain test thread after TearDown returns.
    void TearDown() override {
        ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) { out = IoBufChain{}; });
    }
};

TEST_F(TlsRecordWriterTest, SingleSmallRecordFramedExactly) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto payload = ramp(10, 0x10);
        ASSERT_TRUE(writer.write(TlsContentType::Handshake, make_buf(payload), out).has_value());
        ASSERT_EQ(out.readable_bytes(), 5u + 10u);
        std::vector<std::uint8_t> framed;
        drain(out, framed);
        EXPECT_EQ(framed, concat_bytes({{22, 0x03, 0x03, 0x00, 0x0A}, payload}));
    });
}

TEST_F(TlsRecordWriterTest, SmallRecordMovesPayloadNodeWhole) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto payload = ramp(10, 0x10);
        IoBuf buf = make_buf(payload);
        const std::uint8_t *payload_ptr = buf.readable_data();

        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(buf), out).has_value());

        out.consume(5); // skip the header node
        struct iovec spans[4];
        const int count = out.fill_write_iov(spans, 4);
        ASSERT_EQ(count, 1);
        EXPECT_EQ(spans[0].iov_len, 10u);
        EXPECT_EQ(static_cast<const std::uint8_t *>(spans[0].iov_base), payload_ptr);
    });
}

TEST_F(TlsRecordWriterTest, LegacyVersionIsConfigurable) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        writer.set_legacy_version(0x0301); // conventional first client flight
        ASSERT_TRUE(writer.write(TlsContentType::Handshake, make_buf(ramp(3, 0)), out).has_value());
        std::vector<std::uint8_t> framed;
        drain(out, framed);
        EXPECT_EQ(framed.size(), 8u);
        EXPECT_EQ(framed[1], 0x03);
        EXPECT_EQ(framed[2], 0x01);
    });
}

TEST_F(TlsRecordWriterTest, EmptyPayloadEmitsOneEmptyRecord) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        IoBuf empty;
        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(empty), out).has_value());
        ASSERT_EQ(out.readable_bytes(), 5u);
        std::vector<std::uint8_t> framed;
        drain(out, framed);
        std::vector<ParsedRecord> records;
        parse_records(framed, records);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].type, TlsContentType::ApplicationData);
        EXPECT_EQ(records[0].payload.size(), 0u);
    });
}

TEST_F(TlsRecordWriterTest, ExactMaxSizeStaysOneRecord) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto payload = ramp(kMaxPlain, 0x21);
        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, make_buf(payload), out).has_value());
        EXPECT_EQ(out.readable_bytes(), kMaxPlain + 5);
        std::vector<std::uint8_t> framed;
        drain(out, framed);
        std::vector<ParsedRecord> records;
        parse_records(framed, records);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].payload, payload);
    });
}

TEST_F(TlsRecordWriterTest, MaxPlusOneSplitsIntoTwoRecords) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto payload = ramp(kMaxPlain + 1, 0x22);
        ASSERT_TRUE(writer.write(TlsContentType::Handshake, make_buf(payload), out).has_value());
        std::vector<std::uint8_t> framed;
        drain(out, framed);
        EXPECT_EQ(framed.size(), payload.size() + 10u);
        std::vector<ParsedRecord> records;
        parse_records(framed, records);
        ASSERT_EQ(records.size(), 2u);
        EXPECT_EQ(records[0].type, TlsContentType::Handshake);
        EXPECT_EQ(records[0].payload.size(), kMaxPlain);
        EXPECT_EQ(records[1].payload.size(), 1u);
        EXPECT_EQ(concat_bytes({records[0].payload, records[1].payload}), payload);
    });
}

TEST_F(TlsRecordWriterTest, ChainInputSplicesWholeNodesZeroCopy) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto part1 = ramp(10, 0x00);
        const auto part2 = ramp(20, 0x40);
        const auto part3 = ramp(30, 0x80);
        IoBuf buf1 = make_buf(part1);
        IoBuf buf2 = make_buf(part2);
        IoBuf buf3 = make_buf(part3);
        const std::uint8_t *ptr1 = buf1.readable_data();
        const std::uint8_t *ptr2 = buf2.readable_data();
        const std::uint8_t *ptr3 = buf3.readable_data();

        IoBufChain chain;
        ASSERT_TRUE(chain.append(std::move(buf1)));
        ASSERT_TRUE(chain.append(std::move(buf2)));
        ASSERT_TRUE(chain.append(std::move(buf3)));

        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(chain), out).has_value());

        EXPECT_EQ(chain.readable_bytes(), 0u); // fully drained into out
        out.consume(5);
        struct iovec spans[8];
        const int count = out.fill_write_iov(spans, 8);
        ASSERT_EQ(count, 3);
        EXPECT_EQ(spans[0].iov_len, 10u);
        EXPECT_EQ(spans[1].iov_len, 20u);
        EXPECT_EQ(spans[2].iov_len, 30u);
        EXPECT_EQ(static_cast<const std::uint8_t *>(spans[0].iov_base), ptr1);
        EXPECT_EQ(static_cast<const std::uint8_t *>(spans[1].iov_base), ptr2);
        EXPECT_EQ(static_cast<const std::uint8_t *>(spans[2].iov_base), ptr3);
    });
}

TEST_F(TlsRecordWriterTest, BoundarySplitSharesStorageZeroCopy) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const std::size_t total = kMaxPlain + 3616;
        const auto payload = ramp(total, 0x33);
        IoBuf buf = make_buf(payload);
        const std::uint8_t *payload_ptr = buf.readable_data();

        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(buf), out).has_value());

        // First record's payload is a retained view of the original storage.
        struct iovec spans[8];
        const int count = out.fill_write_iov(spans, 8);
        ASSERT_GE(count, 2);
        EXPECT_EQ(spans[0].iov_len, 5u); // header
        EXPECT_EQ(static_cast<const std::uint8_t *>(spans[1].iov_base), payload_ptr);
        EXPECT_EQ(spans[1].iov_len, kMaxPlain);

        std::vector<std::uint8_t> framed;
        drain(out, framed);
        std::vector<ParsedRecord> records;
        parse_records(framed, records);
        ASSERT_EQ(records.size(), 2u);
        EXPECT_EQ(records[0].payload.size(), kMaxPlain);
        EXPECT_EQ(records[1].payload.size(), 3616u);
        EXPECT_EQ(concat_bytes({records[0].payload, records[1].payload}), payload);
    });
}

TEST_F(TlsRecordWriterTest, NodeChainSplitsAtMisalignedBoundary) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        // 10000 + 10000 nodes: the first record crosses into the second node and
        // the remainder lands below the cap.
        const auto part1 = ramp(10000, 0x00);
        const auto part2 = ramp(10000, 0x60);
        IoBufChain chain;
        ASSERT_TRUE(chain.append(make_buf(part1)));
        ASSERT_TRUE(chain.append(make_buf(part2)));

        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(chain), out).has_value());

        std::vector<std::uint8_t> framed;
        drain(out, framed);
        std::vector<ParsedRecord> records;
        parse_records(framed, records);
        ASSERT_EQ(records.size(), 2u);
        EXPECT_EQ(records[0].payload.size(), kMaxPlain);
        EXPECT_EQ(records[1].payload.size(), 20000u - kMaxPlain);
        EXPECT_EQ(concat_bytes({records[0].payload, records[1].payload}), concat_bytes({part1, part2}));
    });
}

TEST_F(TlsRecordWriterTest, MultipleWritesAppendSequentialRecords) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        const auto alert = std::vector<std::uint8_t>{2, 40}; // fatal, handshake_failure
        const auto data = ramp(4, 0x90);
        ASSERT_TRUE(writer.write(TlsContentType::Alert, make_buf(alert), out).has_value());
        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, make_buf(data), out).has_value());

        std::vector<std::uint8_t> framed;
        drain(out, framed);
        std::vector<ParsedRecord> records;
        parse_records(framed, records);
        ASSERT_EQ(records.size(), 2u);
        EXPECT_EQ(records[0].type, TlsContentType::Alert);
        EXPECT_EQ(records[0].payload, alert);
        EXPECT_EQ(records[1].type, TlsContentType::ApplicationData);
        EXPECT_EQ(records[1].payload, data);
    });
}

TEST_F(TlsRecordWriterTest, OutputFeedReaderRoundTrip) {
    ::fiber::test::run_in_loop([&](::fiber::mem::IoBufNodePool &) {
        // 2.5 records worth of payload split across three nodes, followed by a
        // second write of another content type.
        const std::size_t total = 2 * kMaxPlain + 7000;
        const auto payload = ramp(total, 0x55);
        IoBufChain chain;
        ASSERT_TRUE(chain.append(make_buf(std::vector<std::uint8_t>(payload.begin(), payload.begin() + 9000))));
        ASSERT_TRUE(chain.append(make_buf(std::vector<std::uint8_t>(payload.begin() + 9000, payload.begin() + 25000))));
        ASSERT_TRUE(chain.append(make_buf(std::vector<std::uint8_t>(payload.begin() + 25000, payload.end()))));

        ASSERT_TRUE(writer.write(TlsContentType::ApplicationData, std::move(chain), out).has_value());
        ASSERT_TRUE(writer.write(TlsContentType::Handshake, make_buf(ramp(6, 0xAA)), out).has_value());

        TlsRecordReader reader; // same pool as out's nodes
        ASSERT_TRUE(reader.feed(std::move(out)));

        std::vector<std::uint8_t> reassembled;
        std::size_t app_records = 0;
        std::size_t handshake_records = 0;
        while (reader.pending_bytes() > 0) {
            auto r = reader.next();
            ASSERT_EQ(r.status, TlsRecordReader::Result::Status::Ok);
            if (r.record.type == TlsContentType::ApplicationData) {
                EXPECT_LE(r.record.length, kMaxPlain);
                ++app_records;
            } else {
                ++handshake_records;
            }
            drain(r.record.payload, reassembled);
        }

        // 2.5 payload records + one handshake record.
        EXPECT_EQ(app_records, 3u);
        EXPECT_EQ(handshake_records, 1u);
        EXPECT_EQ(reassembled.size(), total + 6u);
        EXPECT_EQ(std::vector<std::uint8_t>(reassembled.begin(), reassembled.begin() + total), payload);
        EXPECT_EQ(reader.pending_bytes(), 0u);
    });
}

} // namespace
