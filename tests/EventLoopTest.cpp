#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>

TEST(EventLoopTest, GroupIndexMatchesLoopOrder) {
    fiber::event::EventLoopGroup group(3);

    for (std::size_t i = 0; i < group.size(); ++i) {
        auto &loop = group.at(i);
        EXPECT_TRUE(loop.has_group_index());
        EXPECT_EQ(loop.group_index(), i);
    }
}

TEST(EventLoopTest, StandaloneLoopHasNoGroupIndex) {
    fiber::event::EventLoop loop;

    EXPECT_EQ(loop.group(), nullptr);
    EXPECT_FALSE(loop.has_group_index());
}

TEST(EventLoopTest, ImmediateStopDoesNotLoseStopRequest) {
    for (int i = 0; i < 500; ++i) {
        fiber::event::EventLoopGroup group(2);
        group.start();
        group.stop();
        group.join();
    }
}

namespace {
struct BusyDefer {
    fiber::event::EventLoop &loop;
    fiber::event::EventLoop::DeferEntry work{};
    fiber::event::EventLoop::TimerEntry timer{};
    unsigned calls = 0;
    bool timer_fired = false;
    static void run(BusyDefer *self) noexcept {
        ++self->calls;
        self->loop.post_next<BusyDefer, &BusyDefer::work, &BusyDefer::run>(*self);
    }
    static void finish(BusyDefer *self) noexcept {
        self->timer_fired = true;
        self->loop.cancel<BusyDefer, &BusyDefer::work>(*self);
        self->loop.stop();
    }
};
} // namespace

TEST(EventLoopTest, RepostingNextDeferDoesNotStarveTimerOrBlockInPoll) {
    fiber::event::EventLoop loop;
    BusyDefer busy{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        loop.post_next<BusyDefer, &BusyDefer::work, &BusyDefer::run>(busy);
        loop.post_at<BusyDefer, &BusyDefer::timer, &BusyDefer::finish>(loop.now() + std::chrono::milliseconds(2), busy);
        co_return;
    });
    loop.run();
    EXPECT_TRUE(busy.timer_fired);
    EXPECT_GT(busy.calls, 0U);
    EXPECT_FALSE(busy.work.is_in_queue());
}

namespace {
// post_local work runs in the same turn; post_next work runs only after the
// following poll. Event callbacks run directly during that poll, and local work
// they post is queued behind the moved next-turn entries (nginx order).
struct TurnOrder {
    fiber::event::EventLoop &loop;
    fiber::event::EventLoop::DeferEntry first{};
    fiber::event::EventLoop::DeferEntry next{};
    fiber::event::EventLoop::DeferEntry chained{};
    fiber::event::EventLoop::DeferEntry after_poll{};
    int fd = -1;
    std::vector<int> order;
    static void on_first(TurnOrder *self) noexcept {
        self->order.push_back(1);
        // Posted from inside the drain: still this turn, before anything parked
        // for the next turn.
        self->loop.post_local<TurnOrder, &TurnOrder::chained, &TurnOrder::on_chained>(*self);
        self->loop.post_next<TurnOrder, &TurnOrder::next, &TurnOrder::on_next>(*self);
    }
    static void on_chained(TurnOrder *self) noexcept { self->order.push_back(2); }
    static void on_next(TurnOrder *self) noexcept { self->order.push_back(4); }
    static void on_after_poll(TurnOrder *self) noexcept {
        self->order.push_back(5);
        self->loop.stop();
    }
    struct Item : fiber::event::Poller::Item {
        TurnOrder *owner = nullptr;
    } item;
    static void on_event(fiber::event::Poller::Item *raw, int, fiber::event::IoEvent) {
        auto *self = static_cast<Item *>(raw)->owner;
        self->order.push_back(3);
        self->loop.post_local<TurnOrder, &TurnOrder::after_poll, &TurnOrder::on_after_poll>(*self);
        EXPECT_EQ(self->loop.poller().del(self->item), fiber::common::IoErr::None);
    }
};
} // namespace

TEST(EventLoopTest, NextTurnDeferRunsAfterPollBehindLocalWork) {
    fiber::event::EventLoop loop;
    TurnOrder context{loop};
    context.item.owner = &context;
    context.item.callback = &TurnOrder::on_event;
    context.fd = ::eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(context.fd, 0);
    ASSERT_EQ(loop.poller().add(context.fd, fiber::event::IoEvent::Read, &context.item), fiber::common::IoErr::None);
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        loop.post_local<TurnOrder, &TurnOrder::first, &TurnOrder::on_first>(context);
        co_return;
    });
    loop.run();
    ::close(context.fd);
    EXPECT_EQ(context.order, (std::vector<int>{1, 2, 3, 4, 5}));
}

namespace {
struct DeleteEventPeer {
    struct Item : fiber::event::Poller::Item {
        DeleteEventPeer *owner = nullptr;
    };
    fiber::event::EventLoop &loop;
    Item *items[2]{};
    int calls = 0;
    static void on_event(fiber::event::Poller::Item *raw, int, fiber::event::IoEvent) {
        auto *self = static_cast<Item *>(raw)->owner;
        ++self->calls;
        for (auto *&item: self->items) {
            EXPECT_EQ(self->loop.poller().del(*item), fiber::common::IoErr::None);
            ::close(item->fd());
            delete item;
            item = nullptr;
        }
        self->loop.stop();
    }
};
} // namespace

TEST(EventLoopTest, CallbackCanDestroyAnotherItemInSameKernelBatch) {
    fiber::event::EventLoop loop;
    DeleteEventPeer context{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        for (auto *&item: context.items) {
            item = new DeleteEventPeer::Item;
            item->owner = &context;
            item->callback = &DeleteEventPeer::on_event;
            const int fd = ::eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
            EXPECT_GE(fd, 0);
            EXPECT_EQ(loop.poller().add(fd, fiber::event::IoEvent::Read, item), fiber::common::IoErr::None);
        }
        co_return;
    });
    loop.run();
    EXPECT_EQ(context.calls, 1);
}

namespace {
struct ObservePollTime : fiber::event::Poller::Item {
    fiber::event::EventLoop *loop = nullptr;
    std::chrono::steady_clock::time_point signaled_at{};
    std::uint64_t turn_before_wait = 0;
    bool called = false;
    static void on_event(fiber::event::Poller::Item *raw, int, fiber::event::IoEvent) noexcept {
        auto *self = static_cast<ObservePollTime *>(raw);
        self->called = true;
        EXPECT_GE(self->loop->now(), self->signaled_at);
        EXPECT_EQ(self->loop->turn(), self->turn_before_wait + 1);
        EXPECT_EQ(self->loop->poller().del(*self), fiber::common::IoErr::None);
        self->loop->stop();
    }
};
} // namespace

TEST(EventLoopTest, CallbackObservesTimeAndTurnUpdatedAfterWait) {
    fiber::event::EventLoop loop;
    ObservePollTime item;
    item.loop = &loop;
    item.callback = &ObservePollTime::on_event;
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(loop.poller().add(fd, fiber::event::IoEvent::Read, &item), fiber::common::IoErr::None);
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        item.turn_before_wait = loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        item.signaled_at = std::chrono::steady_clock::now();
        const std::uint64_t value = 1;
        EXPECT_EQ(::write(fd, &value, sizeof(value)), static_cast<ssize_t>(sizeof(value)));
        co_return;
    });
    loop.run();
    ::close(fd);
    EXPECT_TRUE(item.called);
}
