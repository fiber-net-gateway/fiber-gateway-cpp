#include <gtest/gtest.h>

#include <string>

#include <fiber/common/mem/BufPool.h>
#include <fiber/http/HttpHeaders.h>

#include "observability/AiServerCatRequest.h"

namespace {

TEST(AiServerCatRequestTest, MapsJavaCompatibleInboundHeaders) {
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    ASSERT_NE(headers.set("Hi-Trace-Id", "root-id"), nullptr);
    ASSERT_NE(headers.set("hi-span-id-parent", "parent-id"), nullptr);
    ASSERT_NE(headers.set("HI-SPAN-ID", "span-id"), nullptr);

    const fiber::cat::MessageTraceContext context = fiber::ai_server::read_cat_trace_context(headers);

    EXPECT_EQ(context.root_message_id, "root-id");
    EXPECT_EQ(context.parent_message_id, "parent-id");
    EXPECT_EQ(context.message_id, "span-id");
}

TEST(AiServerCatRequestTest, InjectsCompleteOutboundContextAndTraceState) {
    const fiber::cat::MessageTraceContext context{
            .message_id = "child-id",
            .root_message_id = "root-id",
            .parent_message_id = "current-id",
    };
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);

    EXPECT_TRUE(fiber::ai_server::inject_cat_headers(headers, &context, "tenant=blue"));
    EXPECT_EQ(headers.get("hi-trace-id"), "root-id");
    EXPECT_EQ(headers.get("hi-span-id-parent"), "current-id");
    EXPECT_EQ(headers.get("hi-span-id"), "child-id");
    EXPECT_EQ(headers.get("tracestate"), "tenant=blue");
}

TEST(AiServerCatRequestTest, RejectsIncompleteOutboundContextWithoutPartialHeaders) {
    const fiber::cat::MessageTraceContext context{
            .message_id = "local-id",
    };
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);

    EXPECT_FALSE(fiber::ai_server::inject_cat_headers(headers, &context));
    EXPECT_FALSE(headers.contains("hi-trace-id"));
    EXPECT_FALSE(headers.contains("hi-span-id-parent"));
    EXPECT_FALSE(headers.contains("hi-span-id"));
}

TEST(AiServerCatRequestTest, DropsOversizedTraceStateWithoutDroppingCatIds) {
    const fiber::cat::MessageTraceContext context{
            .message_id = "child-id",
            .root_message_id = "root-id",
            .parent_message_id = "current-id",
    };
    fiber::mem::BufPool pool;
    fiber::http::HttpHeaders headers(pool);
    const std::string oversized(fiber::ai_server::kMaxAiServerTraceStateBytes + 1, 'x');

    EXPECT_TRUE(fiber::ai_server::inject_cat_headers(headers, &context, oversized));
    EXPECT_EQ(headers.get("hi-trace-id"), "root-id");
    EXPECT_EQ(headers.get("hi-span-id-parent"), "current-id");
    EXPECT_EQ(headers.get("hi-span-id"), "child-id");
    EXPECT_FALSE(headers.contains("tracestate"));
}

} // namespace
