#ifndef FIBER_DNS_DNS_MESSAGE_H
#define FIBER_DNS_DNS_MESSAGE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "../common/IoError.h"
#include "../common/NonCopyable.h"
#include "../common/NonMovable.h"
#include "DnsName.h"
#include "DnsProtocol.h"

namespace fiber::dns {

class MessageParser : public common::NonCopyable, public common::NonMovable {
public:
    struct Question {
        std::string_view name{};
        std::uint16_t type = 0;
        std::uint16_t dns_class = 0;
    };

    struct ResourceRecord {
        std::string_view name{};
        std::uint16_t type = 0;
        std::uint16_t dns_class = 0;
        std::uint32_t ttl = 0;
        const std::uint8_t *rdata = nullptr;
        std::uint16_t rdata_len = 0;
        std::size_t rdata_offset = 0;
    };

    struct MessageView {
        const std::uint8_t *packet_data = nullptr;
        std::size_t packet_len = 0;
        Header header{};
        const Question *questions = nullptr;
        const ResourceRecord *answers = nullptr;
        const ResourceRecord *authorities = nullptr;
        const ResourceRecord *additionals = nullptr;
        std::uint16_t question_count = 0;
        std::uint16_t answer_count = 0;
        std::uint16_t authority_count = 0;
        std::uint16_t additional_count = 0;
    };

    struct Options {
        std::uint16_t max_questions = 2;
        std::uint16_t max_records = 16;
        std::uint16_t max_name_storage = 2048;
    };

    MessageParser() noexcept = default;

    [[nodiscard]] bool init() noexcept { return init(Options{}); }
    [[nodiscard]] bool init(Options options) noexcept;
    void release() noexcept;
    [[nodiscard]] common::IoResult<MessageView> parse(const std::uint8_t *data, std::size_t len) noexcept;

private:
    [[nodiscard]] common::IoResult<std::string_view>
    decode_name_into(const std::uint8_t *data, std::size_t len, std::size_t offset, std::size_t &next_offset) noexcept;
    [[nodiscard]] common::IoErr parse_question(const std::uint8_t *data, std::size_t len, std::size_t &offset,
                                               Question &out) noexcept;
    [[nodiscard]] common::IoErr parse_record(const std::uint8_t *data, std::size_t len, std::size_t &offset,
                                             ResourceRecord &out) noexcept;
    void reset_parse_state(const std::uint8_t *data, std::size_t len) noexcept;

    Options options_{};
    std::unique_ptr<Question[]> questions_{};
    std::unique_ptr<ResourceRecord[]> records_{};
    std::unique_ptr<char[]> name_storage_{};
    std::size_t name_storage_used_ = 0;
    MessageView message_{};
};

[[nodiscard]] common::IoResult<std::size_t> encode_query(const QueryOptions &options, const QuestionSpec &question,
                                                         std::uint8_t *dst, std::size_t cap) noexcept;

} // namespace fiber::dns

#endif // FIBER_DNS_DNS_MESSAGE_H
