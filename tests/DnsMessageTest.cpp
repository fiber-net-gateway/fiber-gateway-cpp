#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>

#include <fiber/common/IoError.h>
#include <fiber/dns/DnsMessage.h>

namespace {

using fiber::common::IoErr;
using fiber::dns::MessageParser;
using fiber::dns::QueryOptions;
using fiber::dns::QuestionSpec;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;

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

TEST(DnsMessageTest, FailWhenNameScratchTooSmall) {
    const std::uint8_t packet[] = {0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                   0x00, 0x03, 'w',  'w',  'w',  0x07, 'e',  'x',  'a',  'm',  'p',
                                   'l',  'e',  0x03, 'c',  'o',  'm',  0x00, 0x00, 0x01, 0x00, 0x01};

    MessageParser parser;
    ASSERT_TRUE(parser.init({1, 0, 4}));
    auto parsed = parser.parse(packet, sizeof(packet));
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error(), IoErr::NoMem);
}

} // namespace
