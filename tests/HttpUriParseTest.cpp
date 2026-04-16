#include <gtest/gtest.h>

#include "common/IoError.h"
#include "common/mem/BufPool.h"
#include "http/HttpUriParse.h"

namespace {

TEST(HttpUriParseTest, FinalizeSimpleOriginFormUri) {
    fiber::http::HttpUriParseState state{};
    ASSERT_EQ(fiber::http::http_scan_origin_form_uri("/assets/app.js?v=42", state), fiber::common::IoErr::None);

    fiber::http::HttpUri uri;
    ASSERT_EQ(fiber::http::http_finalize_request_uri("/assets/app.js?v=42", state, uri), fiber::common::IoErr::None);
    EXPECT_EQ(uri.unparsed_uri, "/assets/app.js?v=42");
    EXPECT_EQ(uri.path, "/assets/app.js");
    EXPECT_EQ(uri.query, "v=42");
    EXPECT_EQ(uri.exten, "js");
}

TEST(HttpUriParseTest, FinalizeComplexUriNormalizesSegmentsAndQuotedBytes) {
    fiber::http::HttpUriParseState state{};
    ASSERT_EQ(fiber::http::http_scan_origin_form_uri("/alpha//beta/../gamma/%64.txt?x=1#frag", state),
              fiber::common::IoErr::None);
    EXPECT_TRUE(state.complex_uri);

    fiber::mem::BufPool pool;
    fiber::http::HttpUri uri;
    ASSERT_EQ(fiber::http::http_finalize_request_uri("/alpha//beta/../gamma/%64.txt?x=1#frag", state, uri, &pool),
              fiber::common::IoErr::None);
    EXPECT_EQ(uri.unparsed_uri, "/alpha//beta/../gamma/%64.txt?x=1#frag");
    EXPECT_EQ(uri.path, "/alpha/gamma/d.txt");
    EXPECT_EQ(uri.query, "x=1");
    EXPECT_EQ(uri.exten, "txt");
}

TEST(HttpUriParseTest, FinalizeEmptyPathInUriPrependsSlash) {
    fiber::http::HttpUriParseState state{};
    state.empty_path_in_uri = true;

    fiber::mem::BufPool pool;
    fiber::http::HttpUri uri;
    ASSERT_EQ(fiber::http::http_finalize_request_uri("?q=1", state, uri, &pool),
              fiber::common::IoErr::None);
    EXPECT_EQ(uri.unparsed_uri, "?q=1");
    EXPECT_EQ(uri.path, "/");
    EXPECT_EQ(uri.query, "q=1");
    EXPECT_TRUE(uri.exten.empty());
}

TEST(HttpUriParseTest, RejectsQuotedNullByte) {
    fiber::http::HttpUriParseState state{};
    ASSERT_EQ(fiber::http::http_scan_origin_form_uri("/a%00b", state), fiber::common::IoErr::None);
    EXPECT_TRUE(state.quoted_uri);

    fiber::mem::BufPool pool;
    fiber::http::HttpUri uri;
    EXPECT_EQ(fiber::http::http_finalize_request_uri("/a%00b", state, uri, &pool),
              fiber::common::IoErr::Invalid);
}

} // namespace
