#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/route/RoutePathMatcher.h"

namespace {

using fiber::common::route::RoutePathMatcher;
using fiber::common::route::RoutePatternError;

struct TestRoute {
    int token = -1;
    std::vector<std::string> path_var_names;
};

struct TestVarDefiner {
    std::map<int, std::uint32_t> mounted_node_ids;
    std::map<int, std::string> mounted_paths;

    void add_path_var_definer(TestRoute &route, std::string_view var_name, std::uint32_t idx) {
        ASSERT_EQ(route.path_var_names.size(), idx);
        route.path_var_names.emplace_back(var_name);
    }

    int on_route_mount(std::uint32_t route_node_id, std::string_view full_path, TestRoute &route) {
        EXPECT_TRUE(mounted_node_ids.emplace(route.token, route_node_id).second);
        EXPECT_TRUE(mounted_paths.emplace(route.token, std::string(full_path)).second);
        return route.token;
    }
};

class Tester {
public:
    Tester() : builder_(definer_) {}

    int add_path(std::string_view pattern) {
        const int token = next_token_++;
        builder_.add_route(pattern, TestRoute{.token = token});
        return token;
    }

    void expect_match(std::string_view path, int expected_token) {
        expect_match(path, expected_token, {});
    }

    void expect_match(std::string_view path, int expected_token, std::map<std::string, std::string> expected_vars) {
        matched_token_ = expected_token;
        ensure_built();
        ASSERT_TRUE(matcher_.match_path(path, *this));
        for (const auto &[name, value] : path_vars_) {
            auto it = expected_vars.find(name);
            ASSERT_NE(it, expected_vars.end());
            EXPECT_EQ(it->second, value);
            expected_vars.erase(it);
        }
        EXPECT_TRUE(expected_vars.empty());
        path_vars_.clear();
    }

    void expect_unmatched(std::string_view path) {
        ensure_built();
        EXPECT_FALSE(matcher_.match_path(path, *this));
        path_vars_.clear();
    }

    bool matched(std::uint32_t, const int &handler) {
        return handler == matched_token_;
    }

    void add_path_var(std::string_view name, std::string_view value) {
        path_vars_.emplace_back(std::string(name), std::string(value));
    }

    void pop_path_var() {
        ASSERT_FALSE(path_vars_.empty());
        path_vars_.pop_back();
    }

    [[nodiscard]] std::uint32_t max_path_var_count() const {
        return matcher_.max_path_var_count();
    }

    [[nodiscard]] const std::string &mounted_path(int token) {
        ensure_built();
        return definer_.mounted_paths.at(token);
    }

    [[nodiscard]] std::uint32_t mounted_node_id(int token) {
        ensure_built();
        return definer_.mounted_node_ids.at(token);
    }

private:
    void ensure_built() {
        if (!built_) {
            matcher_ = builder_.build();
            built_ = true;
        }
    }

    TestVarDefiner definer_{};
    RoutePathMatcher<int>::Builder<TestRoute, TestVarDefiner> builder_;
    RoutePathMatcher<int> matcher_{};
    std::vector<std::pair<std::string, std::string>> path_vars_{};
    int matched_token_ = -1;
    int next_token_ = 0;
    bool built_ = false;
};

TEST(RoutePathMatcherTest, MatchesStaticWildcardAndTrailingSlash) {
    {
        Tester tester;
        const int slash = tester.add_path("/a/b/");
        const int tail = tester.add_path("/a/b/*tail");
        const int exact = tester.add_path("/a/b");
        tester.expect_match("/a/b", exact);
        tester.expect_match("/a/b/", slash);
        tester.expect_match("/a/b/aa", tail, {{"tail", "aa"}});
    }

    {
        Tester tester;
        const int slash = tester.add_path("/a/b/");
        const int tail = tester.add_path("/a/b/*tail");
        tester.expect_match("/a/b", slash);
        tester.expect_match("/a/b/", slash);
        tester.expect_match("/a/b/aa", tail, {{"tail", "aa"}});
        tester.expect_match("/a/b/aa/bb", tail, {{"tail", "aa/bb"}});
        tester.expect_match("/a/b/aa/bb/cc.ccc", tail, {{"tail", "aa/bb/cc.ccc"}});
    }
}

TEST(RoutePathMatcherTest, MatchesPlaceholderAndFallbackOrdering) {
    {
        Tester tester;
        const int slash = tester.add_path("/a/b/");
        const int tail = tester.add_path("/a/b/*tail");
        const int placeholder = tester.add_path("/a/b/:ph");
        const int exact = tester.add_path("/a/b");
        tester.expect_match("/a/b", exact);
        tester.expect_match("/a/b/", slash);
        tester.expect_match("/a/b/aa", placeholder, {{"ph", "aa"}});
        tester.expect_match("/a/b/aa/", placeholder, {{"ph", "aa"}});
        tester.expect_match("/a/b/aa/xx", tail, {{"tail", "aa/xx"}});
        tester.expect_unmatched("/a");
        tester.expect_unmatched("/a/c");
    }

    {
        Tester tester;
        const int placeholder = tester.add_path("/a/b/:xx");
        tester.expect_unmatched("/a/b");
        tester.expect_match("/a/b/", placeholder, {{"xx", ""}});
        tester.expect_match("/a/b/aa", placeholder, {{"xx", "aa"}});
        tester.expect_unmatched("/a/b/aa/cc");
    }
}

TEST(RoutePathMatcherTest, CollapsesRepeatedSlashesLikeJavaMatcher) {
    {
        Tester tester;
        const int root = tester.add_path("/");
        const int wildcard = tester.add_path("/*");
        tester.expect_match("/a/b", wildcard);
        tester.expect_match("//", root);
        tester.expect_match("/", root);
    }

    {
        Tester tester;
        const int collapsed = tester.add_path("//a///b///*");
        const int fallback = tester.add_path("/*");
        const int named_tail = tester.add_path("/cc/dd/:ee/*tail");
        tester.expect_match("/a/b", collapsed);
        tester.expect_match("//", fallback);
        tester.expect_match("/", fallback);
        tester.expect_match("/cc/dd/ee1/", named_tail, {{"ee", "ee1"}, {"tail", ""}});
    }
}

TEST(RoutePathMatcherTest, ReportsMaxPathVarCount) {
    Tester tester;
    tester.add_path("/a/:first/b/:second/*tail");
    tester.add_path("/x");
    tester.expect_match("/x", 1);
    EXPECT_EQ(tester.max_path_var_count(), 3u);
}

TEST(RoutePathMatcherTest, RejectsWildcardInMiddleSegment) {
    TestVarDefiner definer;
    RoutePathMatcher<int>::Builder<TestRoute, TestVarDefiner> builder(definer);
    EXPECT_THROW(builder.add_route("/a/*tail/b", TestRoute{.token = 1}), RoutePatternError);
}

TEST(RoutePathMatcherTest, RejectsNonAsciiPattern) {
    TestVarDefiner definer;
    RoutePathMatcher<int>::Builder<TestRoute, TestVarDefiner> builder(definer);
    EXPECT_THROW(builder.add_route("/路由", TestRoute{.token = 1}), RoutePatternError);
}

TEST(RoutePathMatcherTest, HandlesLargeStaticRouteSets) {
    Tester tester;
    std::map<std::string, int> routes;
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 100; ++j) {
            std::string pattern = "/a/" + std::to_string(i) + "/b/" + std::to_string(j);
            routes.emplace(pattern, tester.add_path(pattern));
        }
    }

    for (int round = 0; round < 100; ++round) {
        for (const auto &[pattern, token] : routes) {
            tester.expect_match(pattern, token);
        }
    }
}

TEST(RoutePathMatcherTest, PreservesOriginalPatternPerMountedRoute) {
    Tester tester;
    const int canonical = tester.add_path("/a/b");
    const int collapsed = tester.add_path("//a///b");

    tester.expect_match("/a/b", canonical);
    tester.expect_match("/a/b", collapsed);
    EXPECT_EQ(tester.mounted_path(canonical), "/a/b");
    EXPECT_EQ(tester.mounted_path(collapsed), "//a///b");
    EXPECT_EQ(tester.mounted_node_id(canonical), tester.mounted_node_id(collapsed));
}

} // namespace
