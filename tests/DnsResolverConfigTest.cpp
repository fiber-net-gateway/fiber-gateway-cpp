#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <string_view>

#include <fiber/async/Spawn.h>
#include <fiber/dns/DnsResolverConfig.h>
#include <fiber/event/EventLoopGroup.h>

namespace {

using fiber::dns::ResolverConfigErrorCode;
using fiber::dns::ResolverUnsupportedFeature;

TEST(DnsResolverConfigTest, ParsesOrderedNameserversAndSupportedOptions) {
    constexpr std::string_view text = R"(
# generated fixture
nameserver 192.0.2.1
nameserver 2001:db8::53
options timeout:7 attempts:4 rotate
)";

    auto parsed = fiber::dns::parse_resolver_config(text);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->nameservers.size(), 2u);
    EXPECT_EQ(parsed->nameservers[0].ip().to_string(), "192.0.2.1");
    EXPECT_EQ(parsed->nameservers[0].port(), 53u);
    EXPECT_EQ(parsed->nameservers[1].ip().to_string(), "2001:db8::53");
    EXPECT_EQ(parsed->timeout, std::chrono::seconds(7));
    EXPECT_EQ(parsed->attempts, 4u);
    EXPECT_TRUE(parsed->rotate);
    EXPECT_EQ(parsed->unsupported, ResolverUnsupportedFeature::None);
}

TEST(DnsResolverConfigTest, RetainsSearchAndReportsUnsupportedFeatures) {
    constexpr std::string_view text = R"(
search ignored.example old.example
domain final.example
nameserver 127.0.0.1
sortlist 10.0.0.0/8
options ndots:5 edns0 trust-ad unknown-option
lookup file bind
)";

    auto parsed = fiber::dns::parse_resolver_config(text);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->search.size(), 1u);
    EXPECT_EQ(parsed->search[0], "final.example");
    EXPECT_EQ(parsed->ndots, 5u);
    EXPECT_TRUE(fiber::dns::has_unsupported_feature(parsed->unsupported, ResolverUnsupportedFeature::Search));
    EXPECT_TRUE(fiber::dns::has_unsupported_feature(parsed->unsupported, ResolverUnsupportedFeature::Ndots));
    EXPECT_TRUE(fiber::dns::has_unsupported_feature(parsed->unsupported, ResolverUnsupportedFeature::SortList));
    EXPECT_TRUE(fiber::dns::has_unsupported_feature(parsed->unsupported, ResolverUnsupportedFeature::Option));
    EXPECT_TRUE(fiber::dns::has_unsupported_feature(parsed->unsupported, ResolverUnsupportedFeature::Directive));
    EXPECT_EQ(parsed->first_unsupported_line, 2u);
}

TEST(DnsResolverConfigTest, EnforcesNameserverAndOptionBounds) {
    auto too_many = fiber::dns::parse_resolver_config(R"(
nameserver 192.0.2.1
nameserver 192.0.2.2
nameserver 192.0.2.3
nameserver 192.0.2.4
)");
    ASSERT_FALSE(too_many.has_value());
    EXPECT_EQ(too_many.error().code, ResolverConfigErrorCode::TooManyNameservers);
    EXPECT_EQ(too_many.error().line, 5u);

    auto invalid_option = fiber::dns::parse_resolver_config("nameserver 127.0.0.1\noptions attempts:6\n");
    ASSERT_FALSE(invalid_option.has_value());
    EXPECT_EQ(invalid_option.error().code, ResolverConfigErrorCode::InvalidOption);

    auto no_nameserver = fiber::dns::parse_resolver_config("search example.test\n");
    ASSERT_FALSE(no_nameserver.has_value());
    EXPECT_EQ(no_nameserver.error().code, ResolverConfigErrorCode::NoNameserver);
}

TEST(DnsResolverConfigTest, RejectsInvalidAndUnsafeNameserverAddresses) {
    auto invalid = fiber::dns::parse_resolver_config("nameserver not-an-ip\n");
    ASSERT_FALSE(invalid.has_value());
    EXPECT_EQ(invalid.error().code, ResolverConfigErrorCode::InvalidNameserver);

    auto unspecified = fiber::dns::parse_resolver_config("nameserver 0.0.0.0\n");
    ASSERT_FALSE(unspecified.has_value());
    EXPECT_EQ(unspecified.error().code, ResolverConfigErrorCode::InvalidNameserver);

    auto multicast = fiber::dns::parse_resolver_config("nameserver 224.0.0.1\n");
    ASSERT_FALSE(multicast.has_value());
    EXPECT_EQ(multicast.error().code, ResolverConfigErrorCode::InvalidNameserver);
}

TEST(DnsResolverConfigTest, SynchronousLoaderRejectsEventLoopThreads) {
    fiber::event::EventLoopGroup group(1);
    group.start();
    std::promise<ResolverConfigErrorCode> result_promise;

    fiber::async::spawn(group.at(0), [&]() -> fiber::async::DetachedTask {
        auto result = fiber::dns::load_system_resolver_config();
        result_promise.set_value(result ? ResolverConfigErrorCode::InvalidArgument : result.error().code);
        co_return;
    });

    EXPECT_EQ(result_promise.get_future().get(), ResolverConfigErrorCode::CalledFromEventLoop);
    group.stop();
    group.join();
}

} // namespace
