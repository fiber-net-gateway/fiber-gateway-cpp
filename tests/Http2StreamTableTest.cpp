#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#define private public
#include <fiber/http/Http2StreamTable.h>
#undef private
#include "Http2TestSupport.h"

namespace {

std::size_t hash_stream_id(std::uint32_t stream_id) {
    std::uint32_t value = stream_id * 2654435761u;
    value ^= value >> 16;
    return value;
}

std::array<std::uint32_t, 3> find_colliding_stream_ids(std::size_t bucket_count) {
    std::array<std::uint32_t, 3> out{};
    std::size_t target_bucket = hash_stream_id(1) & (bucket_count - 1);
    std::size_t found = 0;

    for (std::uint32_t stream_id = 1; stream_id < 100000 && found < out.size(); ++stream_id) {
        if ((hash_stream_id(stream_id) & (bucket_count - 1)) == target_bucket) {
            out[found++] = stream_id;
        }
    }

    EXPECT_EQ(found, out.size());
    return out;
}

fiber::http::Http2Stream::Lease make_stream(std::uint32_t stream_id) {
    fiber::http::Http2Stream::Lease stream = TestHttp2StreamOwner::create();
    if (stream) {
        stream->stream_id_ = stream_id;
        // These tests exercise table ownership only. Mark the unattached stream
        // terminal so releasing its final lease can destroy the test owner.
        stream->close();
    }
    return stream;
}

} // namespace

TEST(Http2StreamTableTest, InitializesFixedCapacityFromMaxActiveStreams) {
    fiber::http::Http2StreamTable table;

    ASSERT_TRUE(table.init(3));
    EXPECT_EQ(table.max_active_streams(), 3u);
    EXPECT_EQ(table.bucket_count(), 8u);
    EXPECT_TRUE(table.empty());
}

TEST(Http2StreamTableTest, InsertsFindsAndRejectsDuplicateStreamIds) {
    fiber::http::Http2StreamTable table;
    ASSERT_TRUE(table.init(4));

    fiber::http::Http2Stream::Lease stream1 = make_stream(1);
    fiber::http::Http2Stream::Lease stream3 = make_stream(3);
    fiber::http::Http2Stream::Lease duplicate1 = make_stream(1);
    ASSERT_TRUE(stream1);
    ASSERT_TRUE(stream3);
    ASSERT_TRUE(duplicate1);
    fiber::http::Http2Stream *stream1_ptr = stream1.get();
    fiber::http::Http2Stream *stream3_ptr = stream3.get();

    EXPECT_TRUE(table.insert(std::move(stream1)));
    EXPECT_TRUE(table.insert(std::move(stream3)));
    EXPECT_FALSE(table.insert(std::move(duplicate1)));
    EXPECT_EQ(table.size(), 2u);
    EXPECT_EQ(table.find(1), stream1_ptr);
    EXPECT_EQ(table.find(3), stream3_ptr);
    EXPECT_EQ(table.find(5), nullptr);
}

TEST(Http2StreamTableTest, RejectsInsertPastConfiguredMaxActiveStreams) {
    fiber::http::Http2StreamTable table;
    ASSERT_TRUE(table.init(2));

    fiber::http::Http2Stream::Lease stream1 = make_stream(1);
    fiber::http::Http2Stream::Lease stream3 = make_stream(3);
    fiber::http::Http2Stream::Lease stream5 = make_stream(5);
    ASSERT_TRUE(stream1);
    ASSERT_TRUE(stream3);
    ASSERT_TRUE(stream5);

    EXPECT_TRUE(table.insert(std::move(stream1)));
    EXPECT_TRUE(table.insert(std::move(stream3)));
    EXPECT_FALSE(table.insert(std::move(stream5)));
    EXPECT_EQ(table.size(), 2u);
}

TEST(Http2StreamTableTest, EraseKeepsLaterCollisionsReachable) {
    fiber::http::Http2StreamTable table;
    ASSERT_TRUE(table.init(4));

    auto ids = find_colliding_stream_ids(table.bucket_count());
    fiber::http::Http2Stream::Lease stream_a = make_stream(ids[0]);
    fiber::http::Http2Stream::Lease stream_b = make_stream(ids[1]);
    fiber::http::Http2Stream::Lease stream_c = make_stream(ids[2]);
    ASSERT_TRUE(stream_a);
    ASSERT_TRUE(stream_b);
    ASSERT_TRUE(stream_c);
    fiber::http::Http2Stream *stream_a_ptr = stream_a.get();
    fiber::http::Http2Stream *stream_b_ptr = stream_b.get();
    fiber::http::Http2Stream *stream_c_ptr = stream_c.get();

    ASSERT_TRUE(table.insert(std::move(stream_a)));
    ASSERT_TRUE(table.insert(std::move(stream_b)));
    ASSERT_TRUE(table.insert(std::move(stream_c)));

    fiber::http::Http2Stream::Lease erased_a = table.erase(stream_a_ptr->stream_id());
    EXPECT_EQ(erased_a.get(), stream_a_ptr);
    EXPECT_EQ(table.find(stream_b_ptr->stream_id()), stream_b_ptr);
    EXPECT_EQ(table.find(stream_c_ptr->stream_id()), stream_c_ptr);

    fiber::http::Http2Stream::Lease erased_b = table.erase(stream_b_ptr->stream_id());
    EXPECT_EQ(erased_b.get(), stream_b_ptr);
    EXPECT_EQ(table.find(stream_c_ptr->stream_id()), stream_c_ptr);
    EXPECT_EQ(table.size(), 1u);
}
