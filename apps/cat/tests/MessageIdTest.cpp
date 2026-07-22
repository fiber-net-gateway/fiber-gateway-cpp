#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <fiber/cat/PropagationContext.h>

#include "CatMessageId.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::RecordError;
using fiber::cat::detail::MessageIdGenerator;

TEST(CatMessageIdTest, UsesOfficialVisibleStructureAndChangesHourPrefix) {
    MessageIdGenerator generator("10.2.3.4", 40);
    const auto hour_five = std::chrono::system_clock::time_point(5h);
    auto first = generator.next("checkout", hour_five);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->view(), "checkout-0a020304-5-41");

    auto remote = generator.next("inventory", hour_five);
    ASSERT_TRUE(remote);
    EXPECT_EQ(remote->view(), "inventory-0a020304-5-42");

    auto next_hour = generator.next("checkout", std::chrono::system_clock::time_point(6h));
    ASSERT_TRUE(next_hour);
    EXPECT_EQ(next_hour->view(), "checkout-0a020304-6-43");
}

TEST(CatMessageIdTest, GeneratesUniqueIdsAcrossConcurrentCallers) {
    MessageIdGenerator generator("127.0.0.1", 100);
    std::mutex mutex;
    std::unordered_set<std::string> ids;
    std::vector<std::thread> callers;
    for (std::size_t caller = 0; caller < 4; ++caller) {
        callers.emplace_back([&] {
            for (std::size_t index = 0; index < 500; ++index) {
                auto id = generator.next("app", std::chrono::system_clock::time_point(1h));
                ASSERT_TRUE(id);
                std::lock_guard lock(mutex);
                ids.emplace(id->view());
            }
        });
    }
    for (std::thread &caller: callers) {
        caller.join();
    }
    EXPECT_EQ(ids.size(), 2000);
}

TEST(CatMessageIdTest, OwningPropagationContextCopiesAllFields) {
    std::string message = "child";
    auto created = fiber::cat::PropagationContext::create({
            .message_id = message,
            .root_message_id = "root",
            .parent_message_id = "parent",
            .session_token = "session",
    });
    ASSERT_TRUE(created);
    fiber::cat::PropagationContext context = std::move(*created);
    fiber::cat::PropagationContext copy = context;
    message.assign("xxxxx");

    EXPECT_EQ(copy.message_id(), "child");
    EXPECT_EQ(copy.root_message_id(), "root");
    EXPECT_EQ(copy.parent_message_id(), "parent");
    EXPECT_EQ(copy.session_token(), "session");

    auto invalid = fiber::cat::PropagationContext::create({.root_message_id = "orphan"});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error(), RecordError::InvalidContext);
}

} // namespace
