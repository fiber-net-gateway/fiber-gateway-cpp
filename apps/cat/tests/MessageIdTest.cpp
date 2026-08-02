#include <gtest/gtest.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "CatMessageId.h"

namespace {

using namespace std::chrono_literals;
using fiber::cat::RecordError;
using fiber::cat::detail::MessageIdGenerator;

bool cat3_numeric_fields_parse(std::string_view message_id, std::int32_t *parsed_hour = nullptr,
                               std::int32_t *parsed_index = nullptr) {
    const std::size_t index_separator = message_id.rfind('-');
    if (index_separator == std::string_view::npos || index_separator == 0) {
        return false;
    }
    const std::size_t hour_separator = message_id.rfind('-', index_separator - 1);
    if (hour_separator == std::string_view::npos) {
        return false;
    }

    std::int32_t hour = -1;
    std::int32_t index = -1;
    const auto hour_result =
            std::from_chars(message_id.data() + hour_separator + 1, message_id.data() + index_separator, hour);
    const auto index_result =
            std::from_chars(message_id.data() + index_separator + 1, message_id.data() + message_id.size(), index);
    const bool valid = hour_result.ec == std::errc{} && hour_result.ptr == message_id.data() + index_separator &&
                       hour >= 0 && index_result.ec == std::errc{} &&
                       index_result.ptr == message_id.data() + message_id.size() && index >= 0;
    if (valid && parsed_hour) {
        *parsed_hour = hour;
    }
    if (valid && parsed_index) {
        *parsed_index = index;
    }
    return valid;
}

TEST(CatMessageIdTest, UsesOfficialVisibleStructureAndResetsSequenceOnLaterHour) {
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
    EXPECT_EQ(next_hour->view(), "checkout-0a020304-6-41");

    auto clock_rollback = generator.next("checkout", hour_five);
    ASSERT_TRUE(clock_rollback);
    EXPECT_EQ(clock_rollback->view(), "checkout-0a020304-6-42");
}

TEST(CatMessageIdTest, KeepsIndexWithinCat3LogViewStorageLimit) {
    MessageIdGenerator default_generator("127.0.0.1");
    auto generated = default_generator.next("app-with-dash", std::chrono::system_clock::time_point(1h));
    ASSERT_TRUE(generated);
    std::int32_t generated_index = -1;
    EXPECT_TRUE(cat3_numeric_fields_parse(generated->view(), nullptr, &generated_index));
    EXPECT_LE(generated_index, 1'000'001);

    constexpr std::uint64_t max_cat3_index = 50'000'000U;
    MessageIdGenerator boundary_generator("127.0.0.1", max_cat3_index - 1);
    auto last_valid = boundary_generator.next("app", std::chrono::system_clock::time_point(1h));
    ASSERT_TRUE(last_valid);
    EXPECT_EQ(last_valid->view(), "app-7f000001-1-50000000");
    EXPECT_TRUE(cat3_numeric_fields_parse(last_valid->view()));

    auto exhausted = boundary_generator.next("app", std::chrono::system_clock::time_point(1h));
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error(), RecordError::IdGenerationFailed);

    auto reset_next_hour = boundary_generator.next("app", std::chrono::system_clock::time_point(2h));
    ASSERT_TRUE(reset_next_hour);
    EXPECT_EQ(reset_next_hour->view(), "app-7f000001-2-50000000");
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

} // namespace
