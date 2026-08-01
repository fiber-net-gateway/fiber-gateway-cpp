#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <fiber/nacos/Subscription.h>

#include "../src/SubscriptionPool.h"

namespace {

struct TestProtocolState {};

using TestPool = fiber::nacos::detail::SubscriptionPool<int, TestProtocolState>;
using TestEntry = TestPool::Entry;
using TestEntryPtr = TestPool::EntryPtr;
using TestResult = fiber::nacos::SubscriptionResult<int>;
using TestSubscription = fiber::nacos::Subscription<int>;

struct RemoveContext {
    int id = 0;
    std::vector<int> *calls = nullptr;
    TestSubscription *self = nullptr;
    TestSubscription *next = nullptr;
};

void remove_during_notify(void *context, const TestResult &result) noexcept {
    auto &state = *static_cast<RemoveContext *>(context);
    if (result.kind != fiber::nacos::ResultKind::Success) {
        return;
    }
    state.calls->push_back(state.id);
    if (state.self != nullptr) {
        state.self->close();
    }
    if (state.next != nullptr) {
        state.next->close();
    }
}

void record_value(void *context, const TestResult &result) noexcept {
    if (result.kind == fiber::nacos::ResultKind::Success && result.data) {
        static_cast<std::vector<int> *>(context)->push_back(*result.data);
    }
}

struct CloseContext {
    TestSubscription *subscription = nullptr;
    std::size_t calls = 0;
};

void close_from_closed_callback(void *context, const TestResult &result) noexcept {
    auto &state = *static_cast<CloseContext *>(context);
    if (result.kind == fiber::nacos::ResultKind::Closed) {
        ++state.calls;
        state.subscription->close();
    }
}

struct AddContext {
    TestPool *pool = nullptr;
    std::optional<TestSubscription> *added = nullptr;
    std::vector<int> *calls = nullptr;
};

struct RetainAndCloseContext {
    TestPool *pool = nullptr;
    std::shared_ptr<const int> retained;
    std::vector<fiber::nacos::ResultKind> calls;
};

void retain_and_close(void *context, const TestResult &result) noexcept {
    auto &state = *static_cast<RetainAndCloseContext *>(context);
    state.calls.push_back(result.kind);
    if (result.kind == fiber::nacos::ResultKind::Success) {
        state.retained = result.data;
        state.pool->close_all();
    }
}

void add_during_notify(void *context, const TestResult &result) noexcept {
    auto &state = *static_cast<AddContext *>(context);
    if (result.kind != fiber::nacos::ResultKind::Success || !result.data) {
        return;
    }
    state.calls->push_back(*result.data);
    if (!state.added->has_value()) {
        auto subscription = state.pool->subscribe("data", "group", &record_value, state.calls);
        EXPECT_TRUE(subscription.has_value());
        if (subscription) {
            state.added->emplace(std::move(*subscription));
        }
    }
}

TEST(SubscriptionPoolTest, CallbackCanRemoveCurrentAndNextNode) {
    std::size_t remove_count = 0;
    TestPool pool([](TestEntryPtr) {},
                  [&](TestEntryPtr) {
                      ++remove_count;
                      return fiber::nacos::detail::RemoveDecision::RetireNow;
                  });

    std::vector<int> calls;
    RemoveContext first_context{.id = 1, .calls = &calls};
    RemoveContext second_context{.id = 2, .calls = &calls};
    RemoveContext third_context{.id = 3, .calls = &calls};
    auto first_result = pool.subscribe("data", "group", &remove_during_notify, &first_context);
    auto second_result = pool.subscribe("data", "group", &remove_during_notify, &second_context);
    auto third_result = pool.subscribe("data", "group", &remove_during_notify, &third_context);
    ASSERT_TRUE(first_result);
    ASSERT_TRUE(second_result);
    ASSERT_TRUE(third_result);
    TestSubscription first = std::move(*first_result);
    TestSubscription second = std::move(*second_result);
    TestSubscription third = std::move(*third_result);
    first_context.self = &first;
    first_context.next = &second;

    TestEntryPtr entry = pool.find("data", "group");
    ASSERT_TRUE(entry);
    pool.publish(*entry, std::make_shared<const int>(7));

    EXPECT_EQ(calls, std::vector<int>({1, 3}));
    EXPECT_FALSE(first);
    EXPECT_FALSE(second);
    EXPECT_TRUE(third);
    EXPECT_EQ(remove_count, 0U);

    third.close();
    EXPECT_EQ(remove_count, 1U);
    EXPECT_TRUE(pool.empty());
}

TEST(SubscriptionPoolTest, SubscriberAddedDuringNotifyReceivesVersionOnce) {
    TestPool pool([](TestEntryPtr) {}, [](TestEntryPtr) { return fiber::nacos::detail::RemoveDecision::RetireNow; });
    std::vector<int> calls;
    std::optional<TestSubscription> added;
    AddContext context{.pool = &pool, .added = &added, .calls = &calls};
    auto first = pool.subscribe("data", "group", &add_during_notify, &context);
    ASSERT_TRUE(first);
    TestEntryPtr entry = pool.find("data", "group");
    ASSERT_TRUE(entry);

    pool.publish(*entry, std::make_shared<const int>(11));

    EXPECT_EQ(calls, std::vector<int>({11, 11}));
    first->close();
    added->close();
    EXPECT_TRUE(pool.empty());
}

TEST(SubscriptionPoolTest, LastSubscriberRemovalIgnoresInflightEntryReference) {
    std::size_t remove_count = 0;
    TestPool pool([](TestEntryPtr) {},
                  [&](TestEntryPtr) {
                      ++remove_count;
                      return fiber::nacos::detail::RemoveDecision::RetireNow;
                  });
    std::vector<int> calls;
    auto subscription = pool.subscribe("data", "group", &record_value, &calls);
    ASSERT_TRUE(subscription);
    TestEntryPtr inflight = pool.find("data", "group");
    ASSERT_TRUE(inflight);

    subscription->close();

    EXPECT_EQ(remove_count, 1U);
    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(inflight->pool, nullptr);
}

TEST(SubscriptionPoolTest, IdleEntryCanBeRevivedBeforeRetirement) {
    std::size_t add_count = 0;
    std::size_t remove_count = 0;
    TestPool pool([&](TestEntryPtr) { ++add_count; },
                  [&](TestEntryPtr) {
                      ++remove_count;
                      return fiber::nacos::detail::RemoveDecision::KeepLinked;
                  });
    std::vector<int> calls;
    auto first = pool.subscribe("data", "group", &record_value, &calls);
    ASSERT_TRUE(first);
    TestEntryPtr original = pool.find("data", "group");
    ASSERT_TRUE(original);
    first->close();
    EXPECT_FALSE(pool.empty());

    auto revived = pool.subscribe("data", "group", &record_value, &calls);
    ASSERT_TRUE(revived);
    TestEntryPtr current = pool.find("data", "group");
    EXPECT_EQ(current.get(), original.get());
    EXPECT_EQ(add_count, 2U);
    EXPECT_EQ(remove_count, 1U);

    revived->close();
    EXPECT_EQ(remove_count, 2U);
    pool.retire(*original);
    EXPECT_TRUE(pool.empty());
}

TEST(SubscriptionPoolTest, ShutdownClosesAndOrphansLiveHandle) {
    TestPool pool([](TestEntryPtr) {}, [](TestEntryPtr) { return fiber::nacos::detail::RemoveDecision::RetireNow; });
    std::vector<int> calls;
    auto subscription = pool.subscribe("data", "group", &record_value, &calls);
    ASSERT_TRUE(subscription);

    pool.close_all();

    EXPECT_TRUE(subscription->closed());
    EXPECT_TRUE(pool.empty());
    subscription->close();
}

TEST(SubscriptionPoolTest, ClosedCallbackCanDestroyItsOwnNode) {
    TestPool pool([](TestEntryPtr) {}, [](TestEntryPtr) { return fiber::nacos::detail::RemoveDecision::RetireNow; });
    CloseContext context;
    auto subscribed = pool.subscribe("data", "group", &close_from_closed_callback, &context);
    ASSERT_TRUE(subscribed);
    TestSubscription subscription = std::move(*subscribed);
    context.subscription = &subscription;

    pool.close_all();

    EXPECT_EQ(context.calls, 1U);
    EXPECT_FALSE(subscription);
    EXPECT_TRUE(pool.empty());
}

TEST(SubscriptionPoolTest, RetainedSnapshotSurvivesReentrantShutdownAndEntryRetirement) {
    TestPool pool([](TestEntryPtr) {}, [](TestEntryPtr) { return fiber::nacos::detail::RemoveDecision::RetireNow; });
    RetainAndCloseContext context{.pool = &pool};
    auto subscribed = pool.subscribe("data", "group", &retain_and_close, &context);
    ASSERT_TRUE(subscribed);
    TestEntryPtr entry = pool.find("data", "group");
    ASSERT_TRUE(entry);

    pool.publish(*entry, std::make_shared<const int>(17));
    entry.reset();

    EXPECT_EQ(context.calls, std::vector<fiber::nacos::ResultKind>(
                                     {fiber::nacos::ResultKind::Success, fiber::nacos::ResultKind::Closed}));
    ASSERT_NE(context.retained, nullptr);
    EXPECT_EQ(*context.retained, 17);
    EXPECT_TRUE(subscribed->closed());
    EXPECT_TRUE(pool.empty());
}

} // namespace
