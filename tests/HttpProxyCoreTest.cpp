#include <gtest/gtest.h>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/HttpProxyCore.h>

TEST(HttpProxyCoreTest, ContentLengthSkipsDelayedEmptyCompletionMarker) {
    fiber::http::proxy_core::RequestBodyForwardState state(fiber::http::HttpBodySpec::ContentLength(65536));

    ASSERT_TRUE(state.accepts(65536));
    ASSERT_TRUE(state.should_write(65536));
    state.record_write(65536);

    EXPECT_TRUE(state.complete());
    EXPECT_TRUE(state.accepts(0));
    EXPECT_FALSE(state.should_write(0));
}

TEST(HttpProxyCoreTest, ContentLengthRejectsBytesAfterDeclaredLength) {
    fiber::http::proxy_core::RequestBodyForwardState state(fiber::http::HttpBodySpec::ContentLength(5));

    ASSERT_TRUE(state.accepts(5));
    state.record_write(5);

    EXPECT_FALSE(state.accepts(1));
}

TEST(HttpProxyCoreTest, ZeroContentLengthForwardsCompletionMarker) {
    fiber::http::proxy_core::RequestBodyForwardState state(fiber::http::HttpBodySpec::ContentLength(0));

    EXPECT_TRUE(state.accepts(0));
    EXPECT_TRUE(state.should_write(0));
    state.record_write(0);

    EXPECT_TRUE(state.complete());
    EXPECT_FALSE(state.should_write(0));
}

TEST(HttpProxyCoreTest, ChunkedForwardsEmptyCompletionMarker) {
    fiber::http::proxy_core::RequestBodyForwardState state(fiber::http::HttpBodySpec::Chunked());

    EXPECT_TRUE(state.accepts(0));
    EXPECT_TRUE(state.should_write(0));
    EXPECT_FALSE(state.complete());
}

TEST(HttpProxyCoreTest, ProxyRequestHeaderFilterTerminatesExpect) {
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    auto *expect = headers.add("Expect", "100-continue");
    ASSERT_NE(expect, nullptr);

    EXPECT_TRUE(fiber::http::proxy_core::should_skip_proxy_request_header(headers, *expect));
}
