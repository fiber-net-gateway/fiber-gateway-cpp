#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include <fiber/common/IoError.h>
#include <fiber/dns/DnsMessage.h>

namespace {

using fiber::common::IoErr;
using fiber::dns::MessageParser;
using fiber::dns::QueryOptions;
using fiber::dns::QuestionSpec;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;

void push_be16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void push_be32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_record(std::vector<std::uint8_t> &packet, std::uint16_t type, const std::uint8_t *rdata,
                   std::uint16_t rdata_len) {
    push_be16(packet, 0xc00cU);
    push_be16(packet, type);
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));
    push_be32(packet, 60);
    push_be16(packet, rdata_len);
    packet.insert(packet.end(), rdata, rdata + rdata_len);
}

std::vector<std::uint8_t> make_large_section_response() {
    std::vector<std::uint8_t> packet;
    push_be16(packet, 0x1234U);
    push_be16(packet, 0x8180U);
    push_be16(packet, 1);
    push_be16(packet, 1);
    push_be16(packet, 13);
    push_be16(packet, 10);
    const std::uint8_t qname[] = {3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0};
    packet.insert(packet.end(), std::begin(qname), std::end(qname));
    push_be16(packet, static_cast<std::uint16_t>(RecordType::A));
    push_be16(packet, static_cast<std::uint16_t>(RecordClass::IN));

    const std::uint8_t address[] = {192, 0, 2, 1};
    append_record(packet, static_cast<std::uint16_t>(RecordType::A), address, sizeof(address));
    const std::uint8_t compressed_name[] = {0xc0, 0x0c};
    for (std::uint16_t i = 0; i < 13; ++i) {
        append_record(packet, static_cast<std::uint16_t>(RecordType::NS), compressed_name, sizeof(compressed_name));
    }
    for (std::uint16_t i = 0; i < 10; ++i) {
        append_record(packet, static_cast<std::uint16_t>(RecordType::A), address, sizeof(address));
    }
    return packet;
}

TEST(DnsMessageTest, EncodeQueryWithEdns) {
    std::array<std::uint8_t, 128> buf{};
    QueryOptions options;
    options.id = 0x1234;
    options.recursion_desired = true;
    options.use_edns = true;
    options.max_udp_payload_size = 1232;

    QuestionSpec question;
    question.name = "www.example.com";
    question.type = static_cast<std::uint16_t>(RecordType::A);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

    auto encoded = fiber::dns::encode_query(options, question, buf.data(), buf.size());
    ASSERT_TRUE(encoded.has_value()) << fiber::common::io_err_name(encoded.error());
    ASSERT_EQ(*encoded, 44u);

    EXPECT_EQ(buf[0], 0x12);
    EXPECT_EQ(buf[1], 0x34);
    EXPECT_EQ(buf[2], 0x01);
    EXPECT_EQ(buf[3], 0x00);
    EXPECT_EQ(buf[4], 0x00);
    EXPECT_EQ(buf[5], 0x01);
    EXPECT_EQ(buf[10], 0x00);
    EXPECT_EQ(buf[11], 0x01);

    const std::uint8_t expected_name[] = {3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0};
    EXPECT_EQ(std::memcmp(buf.data() + 12, expected_name, sizeof(expected_name)), 0);
    EXPECT_EQ(buf[29], 0x00);
    EXPECT_EQ(buf[30], 0x01);
    EXPECT_EQ(buf[31], 0x00);
    EXPECT_EQ(buf[32], 0x01);

    EXPECT_EQ(buf[33], 0x00);
    EXPECT_EQ(buf[34], 0x00);
    EXPECT_EQ(buf[35], 0x29);
    EXPECT_EQ(buf[36], 0x04);
    EXPECT_EQ(buf[37], 0xD0);
    EXPECT_EQ(buf[42], 0x00);
    EXPECT_EQ(buf[43], 0x00);
}

TEST(DnsMessageTest, EncodeQueryCarriesEdnsVersionAndDoBit) {
    std::array<std::uint8_t, 128> buf{};
    QueryOptions options;
    options.id = 0xBEEF;
    options.use_edns = true;
    options.edns_version = 1;
    options.dnssec_ok = true;

    QuestionSpec question;
    question.name = "a.test";
    question.type = static_cast<std::uint16_t>(RecordType::AAAA);
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

    auto encoded = fiber::dns::encode_query(options, question, buf.data(), buf.size());
    ASSERT_TRUE(encoded.has_value()) << fiber::common::io_err_name(encoded.error());

    EXPECT_EQ(buf[24], 0x00);
    EXPECT_EQ(buf[25], 0x00);
    EXPECT_EQ(buf[26], 0x29);
    EXPECT_EQ(buf[29], 0x00);
    EXPECT_EQ(buf[30], 0x01);
    EXPECT_EQ(buf[31], 0x80);
    EXPECT_EQ(buf[32], 0x00);
}

TEST(DnsMessageTest, ParseMessageWithCompressedOwnerNames) {
    const std::uint8_t packet[] = {
            0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x03, 'w',  'w',  'w',  0x07, 'e',
            'x',  'a',  'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01, 0xC0, 0x0C, 0x00,
            0x05, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x12, 0x04, 'e',  'd',  'g',  'e',  0x07, 'e',  'x',  'a',
            'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x04, 'e',  'd',  'g',  'e',  0x07, 'e',  'x',  'a',
            'm',  'p',  'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00,
            0x04, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00, 0x29, 0x04, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    MessageParser parser;
    ASSERT_TRUE(parser.init());
    auto parsed = parser.parse(packet, sizeof(packet));
    ASSERT_TRUE(parsed.has_value()) << fiber::common::io_err_name(parsed.error());

    EXPECT_TRUE(parsed->header.is_response());
    EXPECT_EQ(parsed->question_count, 1u);
    EXPECT_EQ(parsed->answer_count, 2u);
    EXPECT_EQ(parsed->additional_count, 1u);
    EXPECT_EQ(parsed->questions[0].name, "www.example.com");
    EXPECT_EQ(parsed->questions[0].type, static_cast<std::uint16_t>(RecordType::A));
    EXPECT_EQ(parsed->answers[0].name, "www.example.com");
    EXPECT_EQ(parsed->answers[0].type, static_cast<std::uint16_t>(RecordType::CNAME));
    EXPECT_EQ(parsed->answers[0].ttl, 60u);
    EXPECT_EQ(parsed->answers[1].name, "edge.example.com");
    EXPECT_EQ(parsed->answers[1].type, static_cast<std::uint16_t>(RecordType::A));
    ASSERT_EQ(parsed->answers[1].rdata_len, 4u);
    EXPECT_EQ(parsed->answers[1].rdata[0], 0x01);
    EXPECT_EQ(parsed->answers[1].rdata[1], 0x02);
    EXPECT_EQ(parsed->answers[1].rdata[2], 0x03);
    EXPECT_EQ(parsed->answers[1].rdata[3], 0x04);
    EXPECT_EQ(parsed->additionals[0].type, static_cast<std::uint16_t>(RecordType::OPT));

    std::array<char, 64> cname_storage{};
    auto cname = fiber::dns::decode_name(parsed->packet_data, parsed->packet_len, parsed->answers[0].rdata_offset,
                                         cname_storage.data(), cname_storage.size());
    ASSERT_TRUE(cname.has_value()) << fiber::common::io_err_name(cname.error());
    EXPECT_EQ(cname->name, "edge.example.com");
}

TEST(DnsMessageTest, RejectCompressionPointerLoop) {
    const std::uint8_t packet[] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                                   0x00, 0x00, 0x00, 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01};

    MessageParser parser;
    ASSERT_TRUE(parser.init());
    auto parsed = parser.parse(packet, sizeof(packet));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), IoErr::Invalid);
}

TEST(DnsMessageTest, ScannerValidatesRecordsBeyondMaterializationCapacity) {
    const auto packet = make_large_section_response();
    MessageParser parser;
    ASSERT_TRUE(parser.init());

    auto scanned = parser.scan(packet.data(), packet.size());
    ASSERT_TRUE(scanned.has_value()) << fiber::common::io_err_name(scanned.error());
    EXPECT_EQ(scanned->answers.count, 1u);
    EXPECT_EQ(scanned->authorities.count, 13u);
    EXPECT_EQ(scanned->additionals.count, 10u);

    MessageParser::RecordCursor cursor = MessageParser::cursor(scanned->answers);
    std::array<char, 256> owner_storage{};
    MessageParser::ResourceRecord record;
    auto next = MessageParser::next_record(*scanned, cursor, owner_storage.data(), owner_storage.size(), record);
    ASSERT_TRUE(next.has_value());
    ASSERT_TRUE(*next);
    EXPECT_EQ(record.name, "www.example.com");
    EXPECT_EQ(record.type, static_cast<std::uint16_t>(RecordType::A));

    auto materialized = parser.parse(packet.data(), packet.size());
    ASSERT_FALSE(materialized.has_value());
    EXPECT_EQ(materialized.error(), IoErr::MessageTooLarge);
}

TEST(DnsMessageTest, ScannerRejectsTruncatedSkippedRecord) {
    auto packet = make_large_section_response();
    packet.pop_back();

    MessageParser parser;
    ASSERT_TRUE(parser.init());
    auto scanned = parser.scan(packet.data(), packet.size());
    ASSERT_FALSE(scanned.has_value());
    EXPECT_EQ(scanned.error(), IoErr::Invalid);
}

TEST(DnsMessageTest, FailWhenNameScratchTooSmall) {
    const std::uint8_t packet[] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x03, 'w',  'w',  'w',  0x07, 'e',  'x',  'a',  'm',  'p',
                                   'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01};

    MessageParser parser;
    ASSERT_TRUE(parser.init({1, 0, 4}));
    auto parsed = parser.parse(packet, sizeof(packet));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), IoErr::MessageTooLarge);
}

} // namespace
