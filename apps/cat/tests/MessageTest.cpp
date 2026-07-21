#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <type_traits>
#include <utility>

#include <async/Sleep.h>
#include <async/Spawn.h>
#include <event/EventLoop.h>
#include <fiber/cat/Cat.h>

namespace {

using namespace std::chrono_literals;
using fiber::cat::MessageKind;
using fiber::cat::RecordError;
using fiber::cat::RecordLimits;
using fiber::cat::Transaction;

template<typename F>
struct LoopCall {
    F callback;
    fiber::event::EventLoop *loop = nullptr;
    fiber::event::EventLoop::NotifyEntry entry{};

    static void run(LoopCall *call) noexcept {
        call->callback();
        call->loop->stop();
    }
};

template<typename F>
void run_on_loop(F &&callback) {
    fiber::event::EventLoop loop;
    LoopCall<std::decay_t<F>> call{.callback = std::forward<F>(callback), .loop = &loop};
    loop.post<LoopCall<std::decay_t<F>>, &LoopCall<std::decay_t<F>>::entry, &LoopCall<std::decay_t<F>>::run>(call);
    loop.run();
}

TEST(CatMessageTest, BuildsNestedTransactionsEventsAndData) {
    run_on_loop([] {
        auto root_result = Transaction::create_root("URL", "/orders");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        EXPECT_EQ(root.add_data("method=GET"), RecordError::None);
        EXPECT_EQ(root.add_data("route", "/orders"), RecordError::None);

        auto child_result = root.start_transaction("SQL", "select-order");
        ASSERT_TRUE(child_result);
        Transaction child = std::move(*child_result);
        EXPECT_EQ(child.set_duration(1500us), RecordError::None);
        EXPECT_EQ(child.log_event("Cache", "miss", "CACHE_MISS", "key=42"), RecordError::None);
        EXPECT_EQ(child.complete(), RecordError::None);
        EXPECT_EQ(root.complete(), RecordError::None);

        const auto root_view = root.view();
        EXPECT_EQ(root_view.kind(), MessageKind::Transaction);
        EXPECT_EQ(root_view.type(), "URL");
        EXPECT_EQ(root_view.name(), "/orders");
        EXPECT_TRUE(root_view.completed());
        EXPECT_TRUE(root_view.success());
        EXPECT_GT(root_view.timestamp_millis(), 0U);
        EXPECT_EQ(root_view.child_count(), 1U);
        EXPECT_EQ(root_view.data_size(), 24U);

        auto data = root_view.data().begin();
        ASSERT_NE(data, root_view.data().end());
        EXPECT_FALSE((*data).key_value);
        EXPECT_EQ((*data).value, "method=GET");
        ++data;
        ASSERT_NE(data, root_view.data().end());
        EXPECT_TRUE((*data).key_value);
        EXPECT_EQ((*data).key, "route");
        EXPECT_EQ((*data).value, "/orders");

        auto children = root_view.children().begin();
        ASSERT_NE(children, root_view.children().end());
        const auto child_view = *children;
        EXPECT_EQ(child_view.kind(), MessageKind::Transaction);
        EXPECT_EQ(child_view.type(), "SQL");
        EXPECT_EQ(child_view.duration(), 1500us);
        ASSERT_EQ(child_view.child_count(), 1U);

        const auto event_view = *child_view.children().begin();
        EXPECT_EQ(event_view.kind(), MessageKind::Event);
        EXPECT_EQ(event_view.type(), "Cache");
        EXPECT_EQ(event_view.name(), "miss");
        EXPECT_EQ(event_view.status(), "CACHE_MISS");
        EXPECT_EQ((*event_view.data().begin()).value, "key=42");
        EXPECT_TRUE(root.tree_ready());
        EXPECT_TRUE(root.tree_has_problem());
        EXPECT_EQ(root.set_status("late"), RecordError::Completed);
        EXPECT_EQ(root.complete(), RecordError::None);
    });
}

TEST(CatMessageTest, WaitsForChildrenCompletedAfterRoot) {
    run_on_loop([] {
        auto root_result = Transaction::create_root("URL", "/parallel");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        auto child_result = root.start_transaction("Call", "backend");
        ASSERT_TRUE(child_result);
        Transaction child = std::move(*child_result);

        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_FALSE(root.tree_ready());
        EXPECT_EQ(child.complete(), RecordError::None);
        EXPECT_TRUE(root.tree_ready());
    });
}

TEST(CatMessageTest, IncompleteHandleBecomesProblem) {
    run_on_loop([] {
        auto root_result = Transaction::create_root("URL", "/incomplete");
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);
        {
            auto child_result = root.start_transaction("Call", "abandoned");
            ASSERT_TRUE(child_result);
            Transaction child = std::move(*child_result);
            EXPECT_FALSE(child.view().completed());
        }

        const auto child_view = *root.view().children().begin();
        EXPECT_TRUE(child_view.completed());
        EXPECT_EQ(child_view.status(), fiber::cat::status::Incomplete);
        EXPECT_TRUE(root.tree_has_problem());
        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_TRUE(root.tree_ready());
    });
}

TEST(CatMessageTest, EnforcesTreeChildAndDataLimits) {
    run_on_loop([] {
        RecordLimits limits;
        limits.max_messages = 2;
        limits.max_children_per_transaction = 1;
        limits.max_data_bytes_per_message = 4;
        auto root_result = Transaction::create_root("T", "root", limits);
        ASSERT_TRUE(root_result);
        Transaction root = std::move(*root_result);

        EXPECT_EQ(root.add_data("12345"), RecordError::LimitExceeded);
        EXPECT_TRUE(root.view().data().empty());
        auto event_result = root.start_event("E", "one");
        ASSERT_TRUE(event_result);
        auto second = root.start_event("E", "two");
        ASSERT_FALSE(second);
        EXPECT_EQ(second.error(), RecordError::LimitExceeded);
        EXPECT_EQ(event_result->complete(), RecordError::None);
        EXPECT_EQ(root.complete(), RecordError::None);
        EXPECT_TRUE(root.tree_ready());
    });
}

TEST(CatMessageTest, SupportsStandaloneEventTree) {
    run_on_loop([] {
        auto event_result = fiber::cat::Event::create_root("Exception", "read failed");
        ASSERT_TRUE(event_result);
        auto event = std::move(*event_result);
        EXPECT_EQ(event.add_data("code", "EIO"), RecordError::None);
        EXPECT_EQ(event.set_timestamp(1234), RecordError::None);
        EXPECT_EQ(event.complete(fiber::cat::status::Error), RecordError::None);
        EXPECT_TRUE(event.tree_ready());
        EXPECT_TRUE(event.tree_has_problem());
        EXPECT_EQ(event.view().timestamp_millis(), 1234U);
        EXPECT_EQ(event.view().status(), fiber::cat::status::Error);
    });
}

struct InterleaveResult {
    std::string root_name;
    std::string event_name;
    std::chrono::microseconds duration{};
    bool ready = false;
};

fiber::async::DetachedTask record_interleaved(std::string_view root_name, std::string_view event_name,
                                              std::chrono::milliseconds delay, InterleaveResult *result,
                                              std::atomic<unsigned int> *done) {
    auto root_result = Transaction::create_root("URL", root_name);
    if (!root_result) {
        done->fetch_add(1, std::memory_order_relaxed);
        co_return;
    }
    Transaction root = std::move(*root_result);
    co_await fiber::async::sleep(delay);
    (void) root.log_event("Marker", event_name);
    (void) root.complete();
    result->root_name = root.view().name();
    result->event_name = (*root.view().children().begin()).name();
    result->duration = root.view().duration();
    result->ready = root.tree_ready();
    if (done->fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
        fiber::event::EventLoop::current().stop();
    }
}

TEST(CatMessageTest, CoroutineInterleavingKeepsTreesIndependent) {
    fiber::event::EventLoop loop;
    std::atomic<unsigned int> done{0};
    InterleaveResult first;
    InterleaveResult second;
    fiber::async::spawn(loop, [&] { return record_interleaved("first", "first-event", 2ms, &first, &done); });
    fiber::async::spawn(loop, [&] { return record_interleaved("second", "second-event", 1ms, &second, &done); });
    loop.run();

    EXPECT_EQ(first.root_name, "first");
    EXPECT_EQ(first.event_name, "first-event");
    EXPECT_GT(first.duration, 0us);
    EXPECT_TRUE(first.ready);
    EXPECT_EQ(second.root_name, "second");
    EXPECT_EQ(second.event_name, "second-event");
    EXPECT_GT(second.duration, 0us);
    EXPECT_TRUE(second.ready);
}

} // namespace
