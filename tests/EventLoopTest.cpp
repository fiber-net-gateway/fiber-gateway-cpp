#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <sys/eventfd.h>
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
        self->loop.post_local<BusyDefer, &BusyDefer::work, &BusyDefer::run>(*self);
    }
    static void finish(BusyDefer *self) noexcept {
        self->timer_fired = true;
        self->loop.cancel<BusyDefer, &BusyDefer::work>(*self);
        self->loop.stop();
    }
};
} // namespace

TEST(EventLoopTest, RepostingDeferDoesNotStarveTimerOrBlockInPoll) {
    fiber::event::EventLoop loop;
    BusyDefer busy{loop};
    fiber::async::spawn(loop, [&]() -> fiber::async::DetachedTask {
        loop.post_local<BusyDefer, &BusyDefer::work, &BusyDefer::run>(busy);
        loop.post_at<BusyDefer, &BusyDefer::timer, &BusyDefer::finish>(loop.now() + std::chrono::milliseconds(2), busy);
        co_return;
    });
    loop.run();
    EXPECT_TRUE(busy.timer_fired);
    EXPECT_GT(busy.calls, 0U);
    EXPECT_FALSE(busy.work.is_in_queue());
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
