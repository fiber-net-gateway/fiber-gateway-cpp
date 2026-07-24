#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "common/mem/IoBuf.h"
#include "http/Http1HeaderParseBuffer.h"
#include "http/Http1Parser.h"
#include "http/generated/Http1HeaderLineParser.h"

namespace {

fiber::mem::IoBuf make_buf(std::string_view data) {
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(data.size());
    if (!buf) {
        return {};
    }
    if (!data.empty()) {
        std::memcpy(buf.writable_data(), data.data(), data.size());
        buf.commit(data.size());
    }
    return buf;
}

std::string_view readable_view(const fiber::mem::IoBuf &buf) {
    return {reinterpret_cast<const char *>(buf.readable_data()), buf.readable()};
}

struct HeaderParseSnapshot {
    fiber::http::ParseCode code;
    std::size_t name_start;
    std::size_t name_end;
    std::size_t value_start;
    std::size_t value_end;
    std::array<std::uint8_t, fiber::http::HeaderLineParser::kLowcaseHeaderLen> lowcase_header;
    std::uint32_t hash;
    std::uint32_t lowcase_index;
    bool invalid_header;

    bool operator==(const HeaderParseSnapshot &) const = default;
};

std::size_t pointer_offset(const std::uint8_t *base, const std::uint8_t *pointer) {
    if (pointer == nullptr) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(pointer - base);
}

HeaderParseSnapshot parse_header_with_split(std::string_view input, std::size_t split) {
    fiber::http::HeaderLineParser parser;
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(input.size());
    EXPECT_TRUE(buf);
    if (!buf) {
        return {};
    }

    if (split > 0) {
        std::memcpy(buf.writable_data(), input.data(), split);
        buf.commit(split);
    }
    fiber::http::ParseCode code = parser.execute(&buf);
    if (code == fiber::http::ParseCode::Again && split < input.size()) {
        const std::size_t remaining = input.size() - split;
        std::memcpy(buf.writable_data(), input.data() + split, remaining);
        buf.commit(remaining);
        code = parser.execute(&buf);
    }

    const auto &state = parser.state();
    HeaderParseSnapshot snapshot{
            .code = code,
            .name_start = pointer_offset(buf.data(), state.header_name_start),
            .name_end = pointer_offset(buf.data(), state.header_name_end),
            .value_start = pointer_offset(buf.data(), state.header_start),
            .value_end = pointer_offset(buf.data(), state.header_end),
            .hash = state.header_hash,
            .lowcase_index = state.lowcase_index,
            .invalid_header = state.invalid_header,
    };
    std::memcpy(snapshot.lowcase_header.data(), state.lowcase_header, snapshot.lowcase_header.size());
    return snapshot;
}

HeaderParseSnapshot parse_header_with_replacement(std::string_view input, std::size_t split) {
    if (split == 0 || split == input.size()) {
        return parse_header_with_split(input, split);
    }

    fiber::http::HeaderLineParser parser;
    fiber::mem::IoBuf first = make_buf(input.substr(0, split));
    EXPECT_TRUE(first);
    fiber::http::ParseCode code = parser.execute(&first);
    EXPECT_EQ(code, fiber::http::ParseCode::Again);

    fiber::mem::IoBuf grown = fiber::mem::IoBuf::allocate(input.size());
    EXPECT_TRUE(grown);
    EXPECT_EQ(parser.replace_buf_ptr(&first, &grown), fiber::http::ParseCode::Ok);

    const std::size_t remaining = input.size() - split;
    std::memcpy(grown.writable_data(), input.data() + split, remaining);
    grown.commit(remaining);
    code = parser.execute(&grown);

    const auto &state = parser.state();
    HeaderParseSnapshot snapshot{
            .code = code,
            .name_start = pointer_offset(grown.data(), state.header_name_start),
            .name_end = pointer_offset(grown.data(), state.header_name_end),
            .value_start = pointer_offset(grown.data(), state.header_start),
            .value_end = pointer_offset(grown.data(), state.header_end),
            .hash = state.header_hash,
            .lowcase_index = state.lowcase_index,
            .invalid_header = state.invalid_header,
    };
    std::memcpy(snapshot.lowcase_header.data(), state.lowcase_header, snapshot.lowcase_header.size());
    return snapshot;
}

HeaderParseSnapshot parse_header_in_chunks(std::string_view input, std::size_t chunk_size) {
    fiber::http::HeaderLineParser parser;
    fiber::mem::IoBuf buf = fiber::mem::IoBuf::allocate(input.size());
    EXPECT_TRUE(buf);
    if (!buf) {
        return {};
    }

    fiber::http::ParseCode code = fiber::http::ParseCode::Again;
    std::size_t offset = 0;
    while (offset < input.size() && code == fiber::http::ParseCode::Again) {
        const std::size_t count = std::min(chunk_size, input.size() - offset);
        std::memcpy(buf.writable_data(), input.data() + offset, count);
        buf.commit(count);
        offset += count;
        code = parser.execute(&buf);
    }

    const auto &state = parser.state();
    HeaderParseSnapshot snapshot{
            .code = code,
            .name_start = pointer_offset(buf.data(), state.header_name_start),
            .name_end = pointer_offset(buf.data(), state.header_name_end),
            .value_start = pointer_offset(buf.data(), state.header_start),
            .value_end = pointer_offset(buf.data(), state.header_end),
            .hash = state.header_hash,
            .lowcase_index = state.lowcase_index,
            .invalid_header = state.invalid_header,
    };
    std::memcpy(snapshot.lowcase_header.data(), state.lowcase_header, snapshot.lowcase_header.size());
    return snapshot;
}

fiber::http::ParseCode parse_header_with_limit(std::string_view input, std::size_t chunk_size) {
    fiber::http::HeaderLineParser parser;
    fiber::http::Http1HeaderParseBuffer storage({
            .init_size = 16,
            .large_size = 16,
            .large_num = 1,
    });
    EXPECT_TRUE(storage.ensure_init());

    std::size_t offset = 0;
    while (offset < input.size()) {
        if (storage.buf().writable() == 0) {
            if (!storage.can_grow()) {
                return fiber::http::ParseCode::HeaderTooLarge;
            }
            EXPECT_TRUE(storage.grow_and_replace(parser));
        }

        const std::size_t count = std::min({chunk_size, input.size() - offset, storage.buf().writable()});
        std::memcpy(storage.buf().writable_data(), input.data() + offset, count);
        storage.buf().commit(count);
        offset += count;

        const fiber::http::ParseCode code = parser.execute(&storage.buf());
        if (code != fiber::http::ParseCode::Again) {
            return code;
        }
    }

    if (storage.buf().writable() == 0 && !storage.can_grow()) {
        return fiber::http::ParseCode::HeaderTooLarge;
    }
    return fiber::http::ParseCode::Again;
}

} // namespace

TEST(Http1HeaderLineGeneratedParserTest, ReportsCompletePartialHeaderDoneAndInvalidInput) {
    fiber_http1_header_line_t parser;
    fiber::http::HeaderLineParser::HeaderLineState state;

    constexpr std::string_view complete = "X-Test: alpha beta   \r\nnext";
    ASSERT_EQ(fiber_http1_header_line_init(&parser), 0);
    parser.data = &state;
    state.header_name_start = reinterpret_cast<std::uint8_t *>(const_cast<char *>(complete.data()));
    EXPECT_EQ(fiber_http1_header_line_execute(&parser, complete.data(), complete.data() + complete.size()),
              FIBER_HTTP1_HEADER_LINE_COMPLETE);
    EXPECT_EQ(parser.error_pos, complete.data() + complete.find("next"));
    EXPECT_EQ(state.header_name_end, state.header_name_start + 6);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(state.header_start),
                               static_cast<std::size_t>(state.header_end - state.header_start)),
              "alpha beta");

    constexpr std::string_view first = "X-Test: alpha\r";
    constexpr std::string_view second = "\n";
    state = {};
    ASSERT_EQ(fiber_http1_header_line_init(&parser), 0);
    parser.data = &state;
    state.header_name_start = reinterpret_cast<std::uint8_t *>(const_cast<char *>(first.data()));
    EXPECT_EQ(fiber_http1_header_line_execute(&parser, first.data(), first.data() + first.size()), 0);
    EXPECT_EQ(fiber_http1_header_line_execute(&parser, second.data(), second.data() + second.size()),
              FIBER_HTTP1_HEADER_LINE_COMPLETE);
    EXPECT_EQ(parser.error_pos, second.data() + second.size());

    constexpr std::string_view header_done = "\r\n";
    state = {};
    ASSERT_EQ(fiber_http1_header_line_init(&parser), 0);
    parser.data = &state;
    state.header_name_start = reinterpret_cast<std::uint8_t *>(const_cast<char *>(header_done.data()));
    EXPECT_EQ(fiber_http1_header_line_execute(&parser, header_done.data(), header_done.data() + header_done.size()),
              FIBER_HTTP1_HEADER_LINE_HEADERS_COMPLETE);

    const std::string nul{"X-Test: alpha\0beta\r\n", 21};
    state = {};
    ASSERT_EQ(fiber_http1_header_line_init(&parser), 0);
    parser.data = &state;
    state.header_name_start = reinterpret_cast<std::uint8_t *>(const_cast<char *>(nul.data()));
    EXPECT_EQ(fiber_http1_header_line_execute(&parser, nul.data(), nul.data() + nul.size()),
              FIBER_HTTP1_HEADER_LINE_INVALID);
    EXPECT_EQ(parser.error_pos, nul.data() + 13);
}

TEST(Http1ParserTest, HeaderLineLlparseMatchesEveryBufferSplitAndReplacement) {
    constexpr std::array inputs{
            std::string_view{"X-Test: alpha beta\r\n"},
            std::string_view{"X-Test: alpha beta   \r\n"},
            std::string_view{"X-Test:    alpha  beta      \r\n"},
            std::string_view{"X-Test:    \r\n"},
            std::string_view{"X-Test:\r\n"},
            std::string_view{"X-Test\r\n"},
            std::string_view{"X_Trace: enabled\r\n"},
            std::string_view{"X@Test: accepted-but-marked\r\n"},
            std::string_view{"Long-Header-Name-That-Wraps-Lowercase-Buffer: value\r\n"},
            std::string_view{"X-Test: \talpha \t beta   \r\n"},
            std::string_view{"X-Test: value\n"},
            std::string_view{"\r\n"},
            std::string_view{"\n"},
    };

    for (const std::string_view input: inputs) {
        const HeaderParseSnapshot contiguous = parse_header_with_split(input, input.size());
        const fiber::http::ParseCode expected =
                input == "\r\n" || input == "\n" ? fiber::http::ParseCode::HeaderDone : fiber::http::ParseCode::Ok;
        ASSERT_EQ(contiguous.code, expected) << input;
        for (std::size_t split = 0; split <= input.size(); ++split) {
            EXPECT_EQ(parse_header_with_split(input, split), contiguous) << "input=" << input << " split=" << split;
            EXPECT_EQ(parse_header_with_replacement(input, split), contiguous)
                    << "replacement input=" << input << " split=" << split;
        }
        for (std::size_t chunk_size = 1; chunk_size <= input.size(); ++chunk_size) {
            EXPECT_EQ(parse_header_in_chunks(input, chunk_size), contiguous)
                    << "input=" << input << " chunk_size=" << chunk_size;
        }
    }
}

TEST(Http1ParserTest, HeaderLineLlparseTrimsOnlyTrailingSpaces) {
    constexpr std::string_view input = "X-Test: \talpha \t beta   \r\n";
    const HeaderParseSnapshot parsed = parse_header_with_split(input, input.size());

    ASSERT_EQ(parsed.code, fiber::http::ParseCode::Ok);
    EXPECT_EQ(input.substr(parsed.value_start, parsed.value_end - parsed.value_start), "\talpha \t beta");
}

TEST(Http1ParserTest, HeaderLineLlparseComputesLowercaseHashAndInvalidNameFlag) {
    constexpr std::string_view valid = "ConTent-Length: 42\r\n";
    const HeaderParseSnapshot parsed = parse_header_in_chunks(valid, 1);
    ASSERT_EQ(parsed.code, fiber::http::ParseCode::Ok);
    ASSERT_EQ(parsed.lowcase_index, 14u);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(parsed.lowcase_header.data()), parsed.lowcase_index),
              "content-length");

    std::uint32_t expected_hash = 0;
    for (unsigned char ch: std::string_view{"content-length"}) {
        expected_hash = expected_hash * 31 + ch;
    }
    EXPECT_EQ(parsed.hash, expected_hash);
    EXPECT_FALSE(parsed.invalid_header);

    constexpr std::string_view invalid_name = "X@Test: value\r\n";
    const HeaderParseSnapshot marked = parse_header_in_chunks(invalid_name, 1);
    EXPECT_EQ(marked.code, fiber::http::ParseCode::Ok);
    EXPECT_TRUE(marked.invalid_header);
}

TEST(Http1ParserTest, HeaderLineLlparseNulMatchesEveryBufferSplit) {
    const std::string input{"X-Test: alpha\0omega\r\n", 21};
    const HeaderParseSnapshot contiguous = parse_header_with_split(input, input.size());
    ASSERT_EQ(contiguous.code, fiber::http::ParseCode::InvalidHeader);

    for (std::size_t split = 0; split <= input.size(); ++split) {
        EXPECT_EQ(parse_header_with_split(input, split), contiguous) << "split=" << split;
    }
}

TEST(Http1ParserTest, HeaderLineInvalidInputMatchesEveryBufferSplit) {
    const std::array inputs{
            std::string{": value\r\n"},
            std::string{" X-Test: value\r\n"},
            std::string{"X\tTest: value\r\n"},
            std::string{"X-Test: value\rX"},
            std::string{"\rX"},
            std::string{"X-Test\0: value\r\n", 16},
    };

    for (const std::string &input: inputs) {
        const HeaderParseSnapshot contiguous = parse_header_with_split(input, input.size());
        ASSERT_EQ(contiguous.code, fiber::http::ParseCode::InvalidHeader);
        for (std::size_t split = 0; split <= input.size(); ++split) {
            EXPECT_EQ(parse_header_with_split(input, split), contiguous) << "input=" << input << " split=" << split;
        }
    }
}

TEST(Http1ParserTest, HeaderLineParserResumesForMultipleLinesInOneBuffer) {
    constexpr std::string_view input = "First: one\r\nSecond: two   \r\n\r\nbody";
    fiber::mem::IoBuf buf = make_buf(input);
    ASSERT_TRUE(buf);
    fiber::http::HeaderLineParser parser;

    ASSERT_EQ(parser.execute(&buf), fiber::http::ParseCode::Ok);
    const auto &first = parser.state();
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first.header_name_start),
                               static_cast<std::size_t>(first.header_name_end - first.header_name_start)),
              "First");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first.header_start),
                               static_cast<std::size_t>(first.header_end - first.header_start)),
              "one");

    ASSERT_EQ(parser.execute(&buf), fiber::http::ParseCode::Ok);
    const auto &second = parser.state();
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second.header_name_start),
                               static_cast<std::size_t>(second.header_name_end - second.header_name_start)),
              "Second");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second.header_start),
                               static_cast<std::size_t>(second.header_end - second.header_start)),
              "two");

    EXPECT_EQ(parser.execute(&buf), fiber::http::ParseCode::HeaderDone);
    EXPECT_EQ(readable_view(buf), "body");
}

TEST(Http1ParserTest, HeaderLineLlparseLengthLimitMatchesEveryChunkSize) {
    const std::string at_limit = "X: " + std::string(27, 'a') + "\r\n";
    const std::string over_limit = "X: " + std::string(28, 'a') + "\r\n";
    ASSERT_EQ(at_limit.size(), 32u);
    ASSERT_GT(over_limit.size(), 32u);

    for (std::size_t chunk_size = 1; chunk_size <= over_limit.size(); ++chunk_size) {
        EXPECT_EQ(parse_header_with_limit(at_limit, chunk_size), fiber::http::ParseCode::Ok)
                << "chunk_size=" << chunk_size;
        EXPECT_EQ(parse_header_with_limit(over_limit, chunk_size), fiber::http::ParseCode::HeaderTooLarge)
                << "chunk_size=" << chunk_size;
    }
}

TEST(Http1ParserTest, ChunkedBodyParserSpansBuffersAndTrailers) {
    fiber::http::BodyParser parser;
    parser.set_chunked();

    fiber::mem::IoBuf first = make_buf("4;foo=bar\r\nWi");
    ASSERT_TRUE(first);
    EXPECT_EQ(parser.execute(&first), fiber::http::ParseCode::Ok);
    EXPECT_EQ(parser.remaining(), 4u);
    EXPECT_EQ(readable_view(first), "Wi");

    first.consume(2);
    parser.consume(2);
    EXPECT_EQ(parser.remaining(), 2u);
    EXPECT_FALSE(parser.done());

    fiber::mem::IoBuf second = make_buf("ki\r\n");
    ASSERT_TRUE(second);
    EXPECT_EQ(parser.execute(&second), fiber::http::ParseCode::Ok);
    EXPECT_EQ(readable_view(second), "ki\r\n");

    second.consume(2);
    parser.consume(2);
    EXPECT_EQ(parser.remaining(), 0u);
    EXPECT_FALSE(parser.done());
    EXPECT_EQ(parser.execute(&second), fiber::http::ParseCode::Again);
    EXPECT_EQ(second.readable(), 0u);

    fiber::mem::IoBuf third = make_buf("5;ext=value\r\npedia\r\n0;done=yes\r\nX-Test: yes\r\n\r\n");
    ASSERT_TRUE(third);
    EXPECT_EQ(parser.execute(&third), fiber::http::ParseCode::Ok);
    EXPECT_EQ(parser.remaining(), 5u);
    EXPECT_EQ(readable_view(third), "pedia\r\n0;done=yes\r\nX-Test: yes\r\n\r\n");

    third.consume(5);
    parser.consume(5);
    EXPECT_EQ(parser.remaining(), 0u);
    EXPECT_FALSE(parser.done());
    EXPECT_EQ(parser.execute(&third), fiber::http::ParseCode::BodyDone);
    EXPECT_FALSE(parser.done());
    EXPECT_EQ(readable_view(third), "X-Test: yes\r\n\r\n");

    parser.finish_chunked_trailers();
    EXPECT_TRUE(parser.done());
    EXPECT_EQ(parser.execute(&third), fiber::http::ParseCode::Done);
}

TEST(Http1ParserTest, ResponseLineParserParsesCompleteStatusLine) {
    fiber::http::ResponseLineParser parser;

    fiber::mem::IoBuf buf = make_buf("HTTP/1.1 200 OK\r\n");
    ASSERT_TRUE(buf);

    EXPECT_EQ(parser.execute(&buf), fiber::http::ParseCode::Ok);

    const auto &state = parser.state();
    EXPECT_EQ(state.http_major, 1);
    EXPECT_EQ(state.http_minor, 1);
    EXPECT_EQ(state.http_version, static_cast<int>(fiber::http::HttpVersion::HTTP_1_1));
    EXPECT_EQ(state.status_code, 200);
    ASSERT_NE(state.status_start, nullptr);
    ASSERT_NE(state.status_end, nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(state.status_start),
                               static_cast<std::size_t>(state.status_end - state.status_start)),
              "200");
    ASSERT_NE(state.reason_start, nullptr);
    ASSERT_NE(state.reason_end, nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(state.reason_start),
                               static_cast<std::size_t>(state.reason_end - state.reason_start)),
              "OK");
    EXPECT_EQ(buf.readable(), 0u);
}

TEST(Http1ParserTest, ResponseLineParserSpansBuffersAndSupportsReplace) {
    fiber::http::ResponseLineParser parser;

    fiber::mem::IoBuf first = make_buf("HTTP/1.1 20");
    ASSERT_TRUE(first);
    EXPECT_EQ(parser.execute(&first), fiber::http::ParseCode::Again);

    fiber::mem::IoBuf grown = fiber::mem::IoBuf::allocate(64);
    ASSERT_TRUE(grown);
    EXPECT_EQ(parser.replace_buf_ptr(&first, &grown), fiber::http::ParseCode::Ok);

    constexpr std::string_view rest = "0 Not Found\r\n";
    std::memcpy(grown.writable_data(), rest.data(), rest.size());
    grown.commit(rest.size());

    EXPECT_EQ(parser.execute(&grown), fiber::http::ParseCode::Ok);

    const auto &state = parser.state();
    EXPECT_EQ(state.http_version, static_cast<int>(fiber::http::HttpVersion::HTTP_1_1));
    EXPECT_EQ(state.status_code, 200);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(state.reason_start),
                               static_cast<std::size_t>(state.reason_end - state.reason_start)),
              "Not Found");
    EXPECT_EQ(grown.readable(), 0u);
}

TEST(Http1ParserTest, ResponseLineParserReplaceDuringReasonParsing) {
    fiber::http::ResponseLineParser parser;

    fiber::mem::IoBuf first = make_buf("HTTP/1.1 200 Not");
    ASSERT_TRUE(first);
    EXPECT_EQ(parser.execute(&first), fiber::http::ParseCode::Again);

    const auto &partial = parser.state();
    ASSERT_NE(partial.reason_start, nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(partial.reason_start),
                               static_cast<std::size_t>(first.readable_data() - partial.reason_start)),
              "Not");
    EXPECT_EQ(partial.reason_end, nullptr);

    fiber::mem::IoBuf grown = fiber::mem::IoBuf::allocate(64);
    ASSERT_TRUE(grown);
    EXPECT_EQ(parser.replace_buf_ptr(&first, &grown), fiber::http::ParseCode::Ok);

    constexpr std::string_view rest = " Found Yet\r\n";
    std::memcpy(grown.writable_data(), rest.data(), rest.size());
    grown.commit(rest.size());

    EXPECT_EQ(parser.execute(&grown), fiber::http::ParseCode::Ok);

    const auto &state = parser.state();
    EXPECT_EQ(state.status_code, 200);
    ASSERT_NE(state.reason_start, nullptr);
    ASSERT_NE(state.reason_end, nullptr);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(state.reason_start),
                               static_cast<std::size_t>(state.reason_end - state.reason_start)),
              "Not Found Yet");
    EXPECT_EQ(grown.readable(), 0u);
}

TEST(Http1ParserTest, ResponseLineParserAcceptsIisStyleStatusExtension) {
    fiber::http::ResponseLineParser parser;

    fiber::mem::IoBuf buf = make_buf("HTTP/1.1 403.1 Forbidden\r\n");
    ASSERT_TRUE(buf);

    EXPECT_EQ(parser.execute(&buf), fiber::http::ParseCode::Ok);

    const auto &state = parser.state();
    EXPECT_EQ(state.status_code, 403);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(state.reason_start),
                               static_cast<std::size_t>(state.reason_end - state.reason_start)),
              ".1 Forbidden");
}

TEST(Http1ParserTest, ResponseLineParserRejectsInvalidPrefix) {
    fiber::http::ResponseLineParser parser;

    fiber::mem::IoBuf buf = make_buf("HTTX/1.1 200 OK\r\n");
    ASSERT_TRUE(buf);

    EXPECT_EQ(parser.execute(&buf), fiber::http::ParseCode::Error);
}

TEST(Http1ParserTest, ChunkedBodyParserRejectsInvalidSizeLine) {
    fiber::http::ChunkedBodyParser parser;
    parser.reset();

    fiber::mem::IoBuf buf = make_buf("+4\r\nWiki\r\n");
    ASSERT_TRUE(buf);
    EXPECT_EQ(parser.execute(&buf), fiber::http::ParseCode::Error);
}

TEST(Http1ParserTest, BodyParserTracksContentLength) {
    fiber::http::BodyParser parser;
    parser.set_content_length(5);

    EXPECT_EQ(parser.type(), fiber::http::BodyParser::Type::ContentLength);
    EXPECT_FALSE(parser.done());
    EXPECT_EQ(parser.remaining(), 5u);
    EXPECT_EQ(parser.execute(nullptr), fiber::http::ParseCode::Ok);

    parser.consume(3);
    EXPECT_FALSE(parser.done());
    EXPECT_EQ(parser.remaining(), 2u);

    parser.consume(2);
    EXPECT_TRUE(parser.done());
    EXPECT_EQ(parser.remaining(), 0u);
    EXPECT_EQ(parser.execute(nullptr), fiber::http::ParseCode::Done);
}
