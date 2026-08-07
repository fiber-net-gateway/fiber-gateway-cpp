#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <fiber/http/SseCursor.h>

namespace {

using fiber::http::SseCursor;
using fiber::http::SseCursorResult;
using fiber::http::SseCursorStatus;

struct CapturedResult {
    SseCursorStatus status = SseCursorStatus::NeedMore;
    std::string fragment;

    bool operator==(const CapturedResult &) const = default;
};

struct CapturedStream {
    std::vector<CapturedResult> results;
    SseCursorStatus terminal = SseCursorStatus::NeedMore;
    bool feed_succeeded = true;
};

bool is_fragment(SseCursorStatus status) {
    return status == SseCursorStatus::FieldName || status == SseCursorStatus::FieldValue ||
           status == SseCursorStatus::Comment;
}

SseCursorStatus drain(SseCursor &cursor, std::vector<CapturedResult> &results) {
    for (;;) {
        SseCursorResult result = cursor.next();
        if (result.status == SseCursorStatus::NeedMore || result.status == SseCursorStatus::Complete) {
            return result.status;
        }
        if (is_fragment(result.status) && !results.empty() && results.back().status == result.status) {
            results.back().fragment.append(result.fragment);
            continue;
        }
        results.push_back({
                .status = result.status,
                .fragment = std::string(result.fragment),
        });
    }
}

CapturedStream capture_chunks(const std::vector<std::string_view> &chunks) {
    SseCursor cursor;
    CapturedStream captured;
    for (std::string_view chunk: chunks) {
        if (chunk.empty()) {
            continue;
        }
        if (!cursor.feed(chunk)) {
            captured.feed_succeeded = false;
            return captured;
        }
        captured.terminal = drain(cursor, captured.results);
        if (captured.terminal != SseCursorStatus::NeedMore) {
            return captured;
        }
    }
    cursor.finish();
    captured.terminal = drain(cursor, captured.results);
    return captured;
}

void expect_result(SseCursor &cursor, SseCursorStatus status, std::string_view fragment = {}) {
    const SseCursorResult result = cursor.next();
    EXPECT_EQ(result.status, status);
    EXPECT_EQ(result.fragment, fragment);
}

TEST(SseCursorTest, ReturnsColonAndTreatsMissingColonAsEmptyValue) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed("data: hello:world\ndata\n\n"));

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::FieldValue, "hello:world");
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::EventEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);

    cursor.finish();
    expect_result(cursor, SseCursorStatus::Complete);
    expect_result(cursor, SseCursorStatus::Complete);
}

TEST(SseCursorTest, RemovesOnlyOneOptionalSpaceAfterColon) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed("data:  value\ndata:\ndata: \n"));

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::FieldValue, " value");
    expect_result(cursor, SseCursorStatus::LineEnd);

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::LineEnd);

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);
}

TEST(SseCursorTest, DistinguishesCommentsFromEventBoundaries) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed(": heartbeat\r:\r\r\n"));

    expect_result(cursor, SseCursorStatus::CommentStart);
    expect_result(cursor, SseCursorStatus::Comment, " heartbeat");
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::CommentStart);
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::EventEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);
}

TEST(SseCursorTest, StreamsEveryFieldPartAcrossChunks) {
    SseCursor cursor;

    ASSERT_TRUE(cursor.feed("da"));
    expect_result(cursor, SseCursorStatus::FieldName, "da");
    expect_result(cursor, SseCursorStatus::NeedMore);

    ASSERT_TRUE(cursor.feed("ta"));
    expect_result(cursor, SseCursorStatus::FieldName, "ta");
    expect_result(cursor, SseCursorStatus::NeedMore);

    ASSERT_TRUE(cursor.feed(":"));
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::NeedMore);

    ASSERT_TRUE(cursor.feed(" "));
    expect_result(cursor, SseCursorStatus::NeedMore);

    ASSERT_TRUE(cursor.feed("hel"));
    expect_result(cursor, SseCursorStatus::FieldValue, "hel");
    expect_result(cursor, SseCursorStatus::NeedMore);

    ASSERT_TRUE(cursor.feed("lo\r"));
    expect_result(cursor, SseCursorStatus::FieldValue, "lo");
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);

    ASSERT_TRUE(cursor.feed("\n\r\n"));
    expect_result(cursor, SseCursorStatus::EventEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);
}

TEST(SseCursorTest, ReturnsViewsIntoTheFedInput) {
    const std::string input = "data: value";
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed(input));

    SseCursorResult result = cursor.next();
    ASSERT_EQ(result.status, SseCursorStatus::FieldName);
    EXPECT_EQ(result.fragment.data(), input.data());
    EXPECT_EQ(result.fragment, "data");

    expect_result(cursor, SseCursorStatus::Colon);

    result = cursor.next();
    ASSERT_EQ(result.status, SseCursorStatus::FieldValue);
    EXPECT_EQ(result.fragment.data(), input.data() + 6);
    EXPECT_EQ(result.fragment, "value");
    expect_result(cursor, SseCursorStatus::NeedMore);
}

TEST(SseCursorTest, FinishClosesAPartialLineWithoutSynthesizingAnEventBoundary) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed("data: incomplete"));

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::FieldValue, "incomplete");
    expect_result(cursor, SseCursorStatus::NeedMore);

    cursor.finish();
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::Complete);
    EXPECT_TRUE(cursor.input_finished());
    EXPECT_FALSE(cursor.feed("ignored"));
}

TEST(SseCursorTest, FinishCompletesWhenTheLastLineWasAlreadyTerminated) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed("data: complete\n"));

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::FieldValue, "complete");
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);

    cursor.finish();
    expect_result(cursor, SseCursorStatus::Complete);
}

TEST(SseCursorTest, FinishDoesNotRepeatARealEventBoundary) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed("data: complete\n\n"));

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::Colon);
    expect_result(cursor, SseCursorStatus::FieldValue, "complete");
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::EventEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);

    cursor.finish();
    expect_result(cursor, SseCursorStatus::Complete);
}

TEST(SseCursorTest, RejectsFeedUntilCurrentInputIsDrained) {
    SseCursor cursor;
    ASSERT_TRUE(cursor.feed("data"));
    EXPECT_FALSE(cursor.feed("event"));

    expect_result(cursor, SseCursorStatus::FieldName, "data");
    expect_result(cursor, SseCursorStatus::NeedMore);
    EXPECT_TRUE(cursor.feed("\n"));
    expect_result(cursor, SseCursorStatus::LineEnd);
    expect_result(cursor, SseCursorStatus::NeedMore);
}

TEST(SseCursorTest, ProducesTheSameResultsAtEveryChunkBoundary) {
    const std::string input = "data: one\r\nevent\rid: 7\n: ping\r\r\n";
    const CapturedStream expected = capture_chunks({input});
    ASSERT_TRUE(expected.feed_succeeded);
    ASSERT_EQ(expected.terminal, SseCursorStatus::Complete);

    for (std::size_t split = 0; split <= input.size(); ++split) {
        const CapturedStream actual =
                capture_chunks({std::string_view(input).substr(0, split), std::string_view(input).substr(split)});
        EXPECT_TRUE(actual.feed_succeeded) << "split=" << split;
        EXPECT_EQ(actual.terminal, SseCursorStatus::Complete) << "split=" << split;
        EXPECT_EQ(actual.results, expected.results) << "split=" << split;
    }

    std::vector<std::string_view> one_byte_chunks;
    one_byte_chunks.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        one_byte_chunks.emplace_back(input.data() + i, 1);
    }
    const CapturedStream bytewise = capture_chunks(one_byte_chunks);
    EXPECT_TRUE(bytewise.feed_succeeded);
    EXPECT_EQ(bytewise.terminal, SseCursorStatus::Complete);
    EXPECT_EQ(bytewise.results, expected.results);
}

} // namespace
