#include <gtest/gtest.h>

#include <cstring>
#include <string_view>

#include "common/mem/IoBuf.h"
#include "http/Http1Parser.h"

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

} // namespace

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
