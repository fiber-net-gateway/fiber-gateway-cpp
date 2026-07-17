#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "async/Spawn.h"
#include "async/Timeout.h"
#include "common/IoError.h"
#include "dns/DnsClient.h"
#include "dns/DnsMessage.h"
#include "dns/DnsName.h"
#include "event/EventLoop.h"
#include "net/IpAddress.h"
#include "net/SocketAddress.h"
#include "net/TcpStream.h"
#include "net/UdpSocket.h"

namespace {

using fiber::async::DetachedTask;
using fiber::common::IoErr;
using fiber::dns::MessageParser;
using fiber::dns::QuestionSpec;
using fiber::dns::RecordClass;
using fiber::dns::RecordType;

struct CliOptions {
    fiber::net::SocketAddress server{fiber::net::IpAddress::v4({8, 8, 8, 8}), 53};
    std::string qname{};
    std::uint16_t qtype = static_cast<std::uint16_t>(RecordType::A);
    bool force_tcp = false;
    bool short_output = false;
};

std::optional<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    const unsigned long value = std::strtoul(std::string(text).c_str(), &end, 10);
    if (!end || *end != '\0' || value > 65535) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

bool parse_socket_address(std::string_view text, fiber::net::SocketAddress &out) {
    if (text.empty()) {
        return false;
    }

    fiber::net::IpAddress ip;
    if (fiber::net::IpAddress::parse(text, ip)) {
        out = fiber::net::SocketAddress(ip, 53);
        return true;
    }

    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        std::string_view host = text.substr(1, close - 1);
        std::string_view suffix = text.substr(close + 1);
        if (!suffix.empty()) {
            if (suffix.front() != ':') {
                return false;
            }
            suffix.remove_prefix(1);
        }
        std::uint16_t port = 53;
        if (!suffix.empty()) {
            auto parsed_port = parse_port(suffix);
            if (!parsed_port) {
                return false;
            }
            port = *parsed_port;
        }
        if (!fiber::net::IpAddress::parse(host, ip)) {
            return false;
        }
        out = fiber::net::SocketAddress(ip, port);
        return true;
    }

    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || text.find(':') != colon) {
        return false;
    }

    std::string_view host = text.substr(0, colon);
    std::string_view port_text = text.substr(colon + 1);
    auto port = parse_port(port_text);
    if (!port || !fiber::net::IpAddress::parse(host, ip)) {
        return false;
    }
    out = fiber::net::SocketAddress(ip, *port);
    return true;
}

bool parse_server_argument(std::string_view text, fiber::net::SocketAddress &out) {
    if (text.empty() || text.front() != '@') {
        return false;
    }

    text.remove_prefix(1);
    if (text.empty()) {
        return false;
    }

    std::string normalized(text);
    const std::size_t hash = normalized.rfind('#');
    if (hash != std::string::npos) {
        normalized[hash] = ':';
    }
    return parse_socket_address(normalized, out);
}

void override_port(fiber::net::SocketAddress &addr, std::uint16_t port) {
    addr = fiber::net::SocketAddress(addr.ip(), port);
}

std::optional<std::uint16_t> parse_qtype(std::string_view text) {
    std::string normalized(text);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    if (normalized == "A") {
        return static_cast<std::uint16_t>(RecordType::A);
    }
    if (normalized == "AAAA") {
        return static_cast<std::uint16_t>(RecordType::AAAA);
    }
    if (normalized == "CNAME") {
        return static_cast<std::uint16_t>(RecordType::CNAME);
    }
    if (normalized == "NS") {
        return static_cast<std::uint16_t>(RecordType::NS);
    }
    if (normalized == "PTR") {
        return static_cast<std::uint16_t>(RecordType::PTR);
    }
    if (normalized == "MX") {
        return static_cast<std::uint16_t>(RecordType::MX);
    }
    if (normalized == "TXT") {
        return static_cast<std::uint16_t>(RecordType::TXT);
    }
    if (normalized == "SOA") {
        return static_cast<std::uint16_t>(RecordType::SOA);
    }
    if (normalized == "SRV") {
        return static_cast<std::uint16_t>(RecordType::SRV);
    }
    if (normalized == "HTTPS") {
        return static_cast<std::uint16_t>(RecordType::HTTPS);
    }
    return std::nullopt;
}

std::string_view record_type_name(std::uint16_t type) {
    switch (static_cast<RecordType>(type)) {
        case RecordType::A:
            return "A";
        case RecordType::NS:
            return "NS";
        case RecordType::CNAME:
            return "CNAME";
        case RecordType::SOA:
            return "SOA";
        case RecordType::PTR:
            return "PTR";
        case RecordType::MX:
            return "MX";
        case RecordType::TXT:
            return "TXT";
        case RecordType::AAAA:
            return "AAAA";
        case RecordType::SRV:
            return "SRV";
        case RecordType::OPT:
            return "OPT";
        case RecordType::HTTPS:
            return "HTTPS";
    }
    return "TYPE?";
}

std::string_view rcode_name(fiber::dns::RCode rcode) {
    using fiber::dns::RCode;
    switch (rcode) {
        case RCode::NoError:
            return "NOERROR";
        case RCode::FormatError:
            return "FORMERR";
        case RCode::ServerFailure:
            return "SERVFAIL";
        case RCode::NxDomain:
            return "NXDOMAIN";
        case RCode::NotImplemented:
            return "NOTIMP";
        case RCode::Refused:
            return "REFUSED";
    }
    return "RCODE?";
}

std::string format_flags(const fiber::dns::Header &header) {
    std::string flags;
    if (header.is_response()) {
        flags += " qr";
    }
    if (header.authoritative_answer()) {
        flags += " aa";
    }
    if (header.truncated()) {
        flags += " tc";
    }
    if (header.recursion_desired()) {
        flags += " rd";
    }
    if (header.recursion_available()) {
        flags += " ra";
    }
    if (header.authentic_data()) {
        flags += " ad";
    }
    if (header.checking_disabled()) {
        flags += " cd";
    }
    if (!flags.empty()) {
        flags.erase(flags.begin());
    }
    return flags;
}

std::string format_ipv4(const std::uint8_t *data) {
    return fiber::net::IpAddress::v4({data[0], data[1], data[2], data[3]}).to_string();
}

std::string format_ipv6(const std::uint8_t *data) {
    return fiber::net::IpAddress::v6({data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8],
                                      data[9], data[10], data[11], data[12], data[13], data[14], data[15]})
            .to_string();
}

std::uint16_t read_be16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

void write_be16(std::uint8_t *dst, std::uint16_t value) {
    dst[0] = static_cast<std::uint8_t>(value >> 8U);
    dst[1] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint32_t read_be32(const std::uint8_t *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U) | (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

fiber::common::IoResult<std::size_t> consume_stream_read(fiber::common::IoResult<std::size_t> result) {
    if (!result) {
        return std::unexpected(result.error());
    }
    if (*result == 0) {
        return std::unexpected(IoErr::ConnReset);
    }
    return result;
}

std::string format_hex(const std::uint8_t *data, std::size_t len) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

std::string format_hex_u16(std::uint16_t value) {
    std::ostringstream out;
    out << std::hex << std::setw(4) << std::setfill('0') << value;
    return out.str();
}

std::string format_txt(const std::uint8_t *data, std::size_t len) {
    std::string out;
    std::size_t offset = 0;
    while (offset < len) {
        const std::size_t chunk = data[offset];
        ++offset;
        if (offset + chunk > len) {
            return format_hex(data, len);
        }
        if (!out.empty()) {
            out += ' ';
        }
        out += '"';
        for (std::size_t i = 0; i < chunk; ++i) {
            const unsigned char ch = data[offset + i];
            if (ch == '"' || ch == '\\') {
                out += '\\';
            }
            out += static_cast<char>(std::isprint(ch) ? ch : '.');
        }
        out += '"';
        offset += chunk;
    }
    return out;
}

std::string decode_name_rdata(const MessageParser::MessageView &message, const MessageParser::ResourceRecord &record) {
    std::array<char, 256> scratch{};
    auto decoded = fiber::dns::decode_name(message.packet_data, message.packet_len, record.rdata_offset, scratch.data(),
                                           scratch.size());
    if (!decoded) {
        return "<invalid-name>";
    }
    return std::string(decoded->name);
}

std::string format_rdata(const MessageParser::MessageView &message, const MessageParser::ResourceRecord &record) {
    switch (static_cast<RecordType>(record.type)) {
        case RecordType::A:
            if (record.rdata_len == 4) {
                return format_ipv4(record.rdata);
            }
            break;
        case RecordType::AAAA:
            if (record.rdata_len == 16) {
                return format_ipv6(record.rdata);
            }
            break;
        case RecordType::CNAME:
        case RecordType::NS:
        case RecordType::PTR:
            return decode_name_rdata(message, record);
        case RecordType::TXT:
            return format_txt(record.rdata, record.rdata_len);
        case RecordType::MX:
            if (record.rdata_len >= 2) {
                std::array<char, 256> scratch{};
                auto exchange = fiber::dns::decode_name(message.packet_data, message.packet_len,
                                                        record.rdata_offset + 2, scratch.data(), scratch.size());
                if (exchange) {
                    return std::to_string(read_be16(record.rdata)) + " " + std::string(exchange->name);
                }
            }
            break;
        case RecordType::SOA:
            if (record.rdata_len >= 20) {
                std::array<char, 256> mname_storage{};
                auto mname = fiber::dns::decode_name(message.packet_data, message.packet_len, record.rdata_offset,
                                                     mname_storage.data(), mname_storage.size());
                if (mname) {
                    std::array<char, 256> rname_storage{};
                    auto rname = fiber::dns::decode_name(message.packet_data, message.packet_len, mname->next_offset,
                                                         rname_storage.data(), rname_storage.size());
                    if (rname && rname->next_offset + 20 <= message.packet_len) {
                        const std::uint8_t *tail = message.packet_data + rname->next_offset;
                        return std::string(mname->name) + " " + std::string(rname->name) + " " +
                               std::to_string(read_be32(tail)) + " " + std::to_string(read_be32(tail + 4)) + " " +
                               std::to_string(read_be32(tail + 8)) + " " + std::to_string(read_be32(tail + 12)) + " " +
                               std::to_string(read_be32(tail + 16));
                    }
                }
            }
            break;
        case RecordType::SRV:
            if (record.rdata_len >= 6) {
                std::array<char, 256> scratch{};
                auto target = fiber::dns::decode_name(message.packet_data, message.packet_len, record.rdata_offset + 6,
                                                      scratch.data(), scratch.size());
                if (target) {
                    return std::to_string(read_be16(record.rdata)) + " " + std::to_string(read_be16(record.rdata + 2)) +
                           " " + std::to_string(read_be16(record.rdata + 4)) + " " + std::string(target->name);
                }
            }
            break;
        case RecordType::OPT:
            return "udp=" + std::to_string(record.dns_class) +
                   " ext-rcode=" + std::to_string((record.ttl >> 24U) & 0xffU) +
                   " version=" + std::to_string((record.ttl >> 16U) & 0xffU) + " flags=0x" +
                   format_hex_u16(static_cast<std::uint16_t>(record.ttl & 0xffffU));
        case RecordType::HTTPS:
            break;
    }
    return format_hex(record.rdata, record.rdata_len);
}

void print_question_section(const MessageParser::MessageView &message) {
    std::cout << "\n;; QUESTION SECTION:\n";
    for (std::size_t i = 0; i < message.question_count; ++i) {
        const auto &question = message.questions[i];
        std::cout << ';' << question.name << "\tIN\t" << record_type_name(question.type) << '\n';
    }
}

void print_short_answers(const MessageParser::MessageView &message) {
    for (std::size_t i = 0; i < message.answer_count; ++i) {
        const auto &record = message.answers[i];
        if (record.type == static_cast<std::uint16_t>(RecordType::OPT)) {
            continue;
        }
        std::cout << format_rdata(message, record) << '\n';
    }
}

void print_record_section(std::string_view title, const MessageParser::MessageView &message,
                          const MessageParser::ResourceRecord *records, std::size_t count) {
    if (count == 0) {
        return;
    }
    std::cout << "\n;; " << title << " SECTION:\n";
    for (std::size_t i = 0; i < count; ++i) {
        const auto &record = records[i];
        if (record.type == static_cast<std::uint16_t>(RecordType::OPT)) {
            std::cout << ".\t" << record.ttl << "\t" << record_type_name(record.type) << "\t"
                      << format_rdata(message, record) << '\n';
            continue;
        }
        std::cout << record.name << "\t" << record.ttl << "\tIN\t" << record_type_name(record.type) << "\t"
                  << format_rdata(message, record) << '\n';
    }
}

void print_usage() {
    std::cerr << "usage: dns_dig [@server[:port]] [-p port] [+tcp] [+short] <name> [type]\n";
    std::cerr << "   or: dns_dig [-p port] [+tcp] [+short] <name> [type] [server[:port]]\n";
    std::cerr << "examples:\n";
    std::cerr << "  dns_dig example.com\n";
    std::cerr << "  dns_dig @1.1.1.1 example.com\n";
    std::cerr << "  dns_dig @1.1.1.1 -p 5353 example.com\n";
    std::cerr << "  dns_dig @1.1.1.1:5353 example.com AAAA\n";
    std::cerr << "  dns_dig @[2606:4700:4700::1111]:53 example.com MX\n";
    std::cerr << "  dns_dig +tcp +short example.com A @1.1.1.1\n";
    std::cerr << "  dns_dig example.com AAAA\n";
    std::cerr << "  dns_dig example.com A 1.1.1.1\n";
    std::cerr << "  dns_dig example.com MX [2606:4700:4700::1111]:53\n";
}

std::optional<CliOptions> parse_args(int argc, char **argv) {
    if (argc < 2) {
        return std::nullopt;
    }

    CliOptions options;
    std::array<std::string_view, 3> positional{};
    std::size_t positional_count = 0;
    std::optional<std::uint16_t> port_override;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg.empty()) {
            return std::nullopt;
        }
        if (arg.front() == '@') {
            if (!parse_server_argument(arg, options.server)) {
                return std::nullopt;
            }
            continue;
        }
        if (arg == "-p") {
            if (i + 1 >= argc) {
                return std::nullopt;
            }
            auto parsed_port = parse_port(argv[++i]);
            if (!parsed_port) {
                return std::nullopt;
            }
            port_override = *parsed_port;
            continue;
        }
        if (arg == "+tcp") {
            options.force_tcp = true;
            continue;
        }
        if (arg == "+short") {
            options.short_output = true;
            continue;
        }
        if (!arg.empty() && (arg.front() == '+' || arg.front() == '-')) {
            return std::nullopt;
        }
        if (positional_count >= positional.size()) {
            return std::nullopt;
        }
        positional[positional_count++] = arg;
    }

    if (positional_count == 0) {
        return std::nullopt;
    }

    options.qname = positional[0];
    if (options.qname.empty()) {
        return std::nullopt;
    }

    if (positional_count >= 2) {
        auto parsed_type = parse_qtype(positional[1]);
        if (parsed_type) {
            options.qtype = *parsed_type;
            if (positional_count == 3 && !parse_socket_address(positional[2], options.server)) {
                return std::nullopt;
            }
            if (positional_count > 3) {
                return std::nullopt;
            }
        } else {
            if (positional_count != 2 || !parse_socket_address(positional[1], options.server)) {
                return std::nullopt;
            }
        }
    }

    if (port_override) {
        override_port(options.server, *port_override);
    }
    return options;
}

fiber::async::Task<fiber::common::IoResult<std::size_t>> query_via_tcp(fiber::event::EventLoop &loop,
                                                                       const CliOptions &options,
                                                                       const QuestionSpec &question, std::uint8_t *dst,
                                                                       std::size_t cap) {
    constexpr std::size_t kRequestCap = 4096;
    std::array<std::uint8_t, kRequestCap> request{};
    fiber::dns::QueryOptions query_options{};
    auto encoded = fiber::dns::encode_query(query_options, question, request.data(), request.size());
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }

    auto connect_result =
            co_await fiber::net::TcpStream::connect(loop, options.server, std::chrono::milliseconds(2000));
    if (!connect_result) {
        co_return std::unexpected(connect_result.error());
    }

    fiber::net::TcpStream stream(std::move(*connect_result));
    std::array<std::uint8_t, 2> len_prefix{};
    write_be16(len_prefix.data(), static_cast<std::uint16_t>(*encoded));

    std::size_t prefix_written = 0;
    while (prefix_written < len_prefix.size()) {
        auto write_result = co_await fiber::async::timeout_for(
                [&]() { return stream.write(len_prefix.data() + prefix_written, len_prefix.size() - prefix_written); },
                std::chrono::milliseconds(2000));
        if (!write_result) {
            stream.close();
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            stream.close();
            co_return std::unexpected(IoErr::ConnReset);
        }
        prefix_written += *write_result;
    }

    std::size_t body_written = 0;
    while (body_written < *encoded) {
        auto write_result = co_await fiber::async::timeout_for(
                [&]() { return stream.write(request.data() + body_written, *encoded - body_written); },
                std::chrono::milliseconds(2000));
        if (!write_result) {
            stream.close();
            co_return std::unexpected(write_result.error());
        }
        if (*write_result == 0) {
            stream.close();
            co_return std::unexpected(IoErr::ConnReset);
        }
        body_written += *write_result;
    }

    std::size_t prefix_read = 0;
    while (prefix_read < len_prefix.size()) {
        auto read_result = co_await fiber::async::timeout_for(
                [&]() { return stream.read(len_prefix.data() + prefix_read, len_prefix.size() - prefix_read); },
                std::chrono::milliseconds(2000));
        auto consumed = consume_stream_read(read_result);
        if (!consumed) {
            stream.close();
            co_return std::unexpected(consumed.error());
        }
        prefix_read += *consumed;
    }

    const std::size_t response_len = read_be16(len_prefix.data());
    if (response_len > cap) {
        stream.close();
        co_return std::unexpected(IoErr::NoMem);
    }

    std::size_t total_read = 0;
    while (total_read < response_len) {
        auto read_result = co_await fiber::async::timeout_for(
                [&]() { return stream.read(dst + total_read, response_len - total_read); },
                std::chrono::milliseconds(2000));
        auto consumed = consume_stream_read(read_result);
        if (!consumed) {
            stream.close();
            co_return std::unexpected(consumed.error());
        }
        total_read += *consumed;
    }

    stream.close();
    co_return response_len;
}

fiber::async::Task<fiber::common::IoResult<std::size_t>> query_via_client(fiber::event::EventLoop &loop,
                                                                          const CliOptions &options,
                                                                          const QuestionSpec &question,
                                                                          std::uint8_t *dst, std::size_t cap) {
    fiber::dns::DnsClient client;
    fiber::dns::DnsClient::Options client_options{};
    client_options.server = options.server;

    if (!client.init(loop, client_options)) {
        co_return std::unexpected(IoErr::Invalid);
    }
    auto result = co_await client.query_raw(question, dst, cap);
    client.close();
    co_return result;
}

std::string transport_name(const CliOptions &options) { return options.force_tcp ? "tcp" : "udp"; }

void print_parsed_response(const CliOptions &options, std::size_t packet_size,
                           const MessageParser::MessageView &message) {
    if (options.short_output) {
        print_short_answers(message);
        return;
    }

    std::cout << ";; server: " << options.server.to_string() << '\n';
    std::cout << ";; transport: " << transport_name(options) << '\n';
    std::cout << ";; opcode: " << static_cast<unsigned int>(message.header.opcode())
              << ", status: " << rcode_name(message.header.rcode()) << ", id: " << message.header.id << '\n';
    std::cout << ";; flags: " << format_flags(message.header) << "; QUERY: " << message.question_count
              << ", ANSWER: " << message.answer_count << ", AUTHORITY: " << message.authority_count
              << ", ADDITIONAL: " << message.additional_count << '\n';
    std::cout << ";; received " << packet_size << " bytes\n";

    print_question_section(message);
    print_record_section("ANSWER", message, message.answers, message.answer_count);
    print_record_section("AUTHORITY", message, message.authorities, message.authority_count);
    print_record_section("ADDITIONAL", message, message.additionals, message.additional_count);
}

DetachedTask run_query(fiber::event::EventLoop *loop, const CliOptions *options, int *exit_code) {
    QuestionSpec question;
    question.name = options->qname;
    question.type = options->qtype;
    question.dns_class = static_cast<std::uint16_t>(RecordClass::IN);

    constexpr std::size_t kResponseCap = 65535;
    auto response = std::make_unique<std::uint8_t[]>(kResponseCap);
    if (!response) {
        std::cerr << "response buffer alloc failed\n";
        *exit_code = 1;
        loop->stop();
        co_return;
    }

    fiber::common::IoResult<std::size_t> query_result = std::unexpected(IoErr::Invalid);
    if (options->force_tcp) {
        query_result = co_await query_via_tcp(*loop, *options, question, response.get(), kResponseCap);
    } else {
        query_result = co_await query_via_client(*loop, *options, question, response.get(), kResponseCap);
    }

    if (!query_result) {
        std::cerr << "query failed: " << fiber::common::io_err_name(query_result.error()) << '\n';
        *exit_code = 1;
        loop->stop();
        co_return;
    }

    MessageParser parser;
    if (!parser.init({2, 64, 4096})) {
        std::cerr << "message parser init failed\n";
        *exit_code = 1;
        loop->stop();
        co_return;
    }

    auto parsed = parser.parse(response.get(), *query_result);
    if (!parsed) {
        std::cerr << "parse failed: " << fiber::common::io_err_name(parsed.error()) << '\n';
        *exit_code = 1;
        loop->stop();
        co_return;
    }

    print_parsed_response(*options, *query_result, *parsed);
    *exit_code = 0;
    loop->stop();
    co_return;
}

} // namespace

int main(int argc, char **argv) {
    auto options = parse_args(argc, argv);
    if (!options) {
        print_usage();
        return 1;
    }

    fiber::event::EventLoop loop;
    int exit_code = 0;
    fiber::async::spawn(loop, [&]() { return run_query(&loop, &*options, &exit_code); });
    loop.run();
    return exit_code;
}
