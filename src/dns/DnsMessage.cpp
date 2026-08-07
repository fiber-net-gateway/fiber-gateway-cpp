#include <fiber/dns/DnsMessage.h>

#include <cstring>

namespace fiber::dns {

namespace {

constexpr std::size_t kDnsHeaderSize = 12;

std::uint16_t read_be16(const std::uint8_t *data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

std::uint32_t read_be32(const std::uint8_t *data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

void write_be16(std::uint8_t *dst, std::uint16_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value >> 8U);
    dst[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_be32(std::uint8_t *dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    dst[1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    dst[2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    dst[3] = static_cast<std::uint8_t>(value & 0xffU);
}

} // namespace

bool MessageParser::init(Options options) noexcept {
    release();
    if (options.max_questions == 0 || options.max_name_storage == 0) {
        return false;
    }
    options_ = options;
    questions_ = std::make_unique<Question[]>(options.max_questions);
    if (!questions_) {
        release();
        return false;
    }
    if (options.max_records != 0) {
        records_ = std::make_unique<ResourceRecord[]>(options.max_records);
        if (!records_) {
            release();
            return false;
        }
    }
    name_storage_ = std::make_unique<char[]>(options.max_name_storage);
    if (!name_storage_) {
        release();
        return false;
    }
    return true;
}

void MessageParser::release() noexcept {
    questions_.reset();
    records_.reset();
    name_storage_.reset();
    options_ = Options{};
    name_storage_used_ = 0;
    message_ = {};
}

void MessageParser::reset_parse_state(const std::uint8_t *data, std::size_t len) noexcept {
    name_storage_used_ = 0;
    message_ = {};
    message_.packet_data = data;
    message_.packet_len = len;
}

common::IoResult<std::string_view> MessageParser::decode_name_into(const std::uint8_t *data, std::size_t len,
                                                                   std::size_t offset,
                                                                   std::size_t &next_offset) noexcept {
    if (!name_storage_) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (name_storage_used_ > options_.max_name_storage) {
        return std::unexpected(common::IoErr::Invalid);
    }

    common::IoResult<DecodedName> result = decode_name(data, len, offset, name_storage_.get() + name_storage_used_,
                                                       options_.max_name_storage - name_storage_used_);
    if (!result) {
        return std::unexpected(result.error());
    }

    next_offset = result->next_offset;
    name_storage_used_ += result->name.size();
    return result->name;
}

common::IoErr MessageParser::parse_question(const std::uint8_t *data, std::size_t len, std::size_t &offset,
                                            Question &out) noexcept {
    std::size_t next_offset = offset;
    auto name = decode_name_into(data, len, offset, next_offset);
    if (!name) {
        return name.error();
    }
    if (next_offset + 4 > len) {
        return common::IoErr::Invalid;
    }

    out.name = *name;
    out.type = read_be16(data + next_offset);
    out.dns_class = read_be16(data + next_offset + 2);
    offset = next_offset + 4;
    return common::IoErr::None;
}

common::IoErr MessageParser::parse_record(const std::uint8_t *data, std::size_t len, std::size_t &offset,
                                          ResourceRecord &out) noexcept {
    std::size_t next_offset = offset;
    auto name = decode_name_into(data, len, offset, next_offset);
    if (!name) {
        return name.error();
    }
    if (next_offset + 10 > len) {
        return common::IoErr::Invalid;
    }

    std::uint16_t rdata_len = read_be16(data + next_offset + 8);
    std::size_t rdata_offset = next_offset + 10;
    if (rdata_offset + rdata_len > len) {
        return common::IoErr::Invalid;
    }

    out.name = *name;
    out.type = read_be16(data + next_offset);
    out.dns_class = read_be16(data + next_offset + 2);
    out.ttl = read_be32(data + next_offset + 4);
    out.rdata = data + rdata_offset;
    out.rdata_len = rdata_len;
    out.rdata_offset = rdata_offset;
    offset = rdata_offset + rdata_len;
    return common::IoErr::None;
}

common::IoResult<MessageParser::MessageView> MessageParser::parse(const std::uint8_t *data, std::size_t len) noexcept {
    if (!questions_ || !name_storage_ || data == nullptr || len < kDnsHeaderSize) {
        return std::unexpected(common::IoErr::Invalid);
    }

    reset_parse_state(data, len);
    Header header;
    header.id = read_be16(data);
    header.flags = read_be16(data + 2);
    header.question_count = read_be16(data + 4);
    header.answer_count = read_be16(data + 6);
    header.authority_count = read_be16(data + 8);
    header.additional_count = read_be16(data + 10);

    std::uint32_t total_records = static_cast<std::uint32_t>(header.answer_count) +
                                  static_cast<std::uint32_t>(header.authority_count) +
                                  static_cast<std::uint32_t>(header.additional_count);
    if (header.question_count > options_.max_questions || total_records > options_.max_records) {
        return std::unexpected(common::IoErr::NoMem);
    }

    std::size_t offset = kDnsHeaderSize;
    for (std::uint16_t i = 0; i < header.question_count; ++i) {
        common::IoErr err = parse_question(data, len, offset, questions_[i]);
        if (err != common::IoErr::None) {
            return std::unexpected(err);
        }
    }

    ResourceRecord *record_cursor = records_.get();
    for (std::uint16_t i = 0; i < total_records; ++i) {
        common::IoErr err = parse_record(data, len, offset, record_cursor[i]);
        if (err != common::IoErr::None) {
            return std::unexpected(err);
        }
    }

    if (offset > len) {
        return std::unexpected(common::IoErr::Invalid);
    }

    message_.header = header;
    message_.questions = header.question_count != 0 ? questions_.get() : nullptr;
    message_.answers = header.answer_count != 0 ? records_.get() : nullptr;
    message_.authorities = header.authority_count != 0 ? records_.get() + header.answer_count : nullptr;
    message_.additionals =
            header.additional_count != 0 ? records_.get() + header.answer_count + header.authority_count : nullptr;
    message_.question_count = header.question_count;
    message_.answer_count = header.answer_count;
    message_.authority_count = header.authority_count;
    message_.additional_count = header.additional_count;
    return message_;
}

common::IoResult<std::size_t> encode_query(const QueryOptions &options, const QuestionSpec &question, std::uint8_t *dst,
                                           std::size_t cap) noexcept {
    if ((dst == nullptr && cap != 0) || question.type == 0 || question.dns_class == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }
    if (cap < kDnsHeaderSize) {
        return std::unexpected(common::IoErr::NoMem);
    }
    if (options.use_edns && options.max_udp_payload_size == 0) {
        return std::unexpected(common::IoErr::Invalid);
    }

    std::uint16_t flags = 0;
    if (options.recursion_desired) {
        flags |= 0x0100U;
    }
    if (options.checking_disabled) {
        flags |= 0x0010U;
    }

    std::memset(dst, 0, kDnsHeaderSize);
    write_be16(dst, options.id);
    write_be16(dst + 2, flags);
    write_be16(dst + 4, 1);
    write_be16(dst + 10, options.use_edns ? 1 : 0);

    std::size_t write_pos = kDnsHeaderSize;
    auto encoded_name = encode_name(question.name, dst + write_pos, cap - write_pos);
    if (!encoded_name) {
        return std::unexpected(encoded_name.error());
    }
    write_pos += *encoded_name;
    if (write_pos + 4 > cap) {
        return std::unexpected(common::IoErr::NoMem);
    }

    write_be16(dst + write_pos, question.type);
    write_be16(dst + write_pos + 2, question.dns_class);
    write_pos += 4;

    if (!options.use_edns) {
        return write_pos;
    }
    if (write_pos + 11 > cap) {
        return std::unexpected(common::IoErr::NoMem);
    }

    dst[write_pos++] = 0;
    write_be16(dst + write_pos, static_cast<std::uint16_t>(RecordType::OPT));
    write_be16(dst + write_pos + 2, options.max_udp_payload_size);
    std::uint32_t opt_ttl = static_cast<std::uint32_t>(options.edns_version) << 16U;
    if (options.dnssec_ok) {
        opt_ttl |= 0x00008000U;
    }
    write_be32(dst + write_pos + 4, opt_ttl);
    write_be16(dst + write_pos + 8, 0);
    write_pos += 10;
    return write_pos;
}

} // namespace fiber::dns
