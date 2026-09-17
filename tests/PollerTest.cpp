#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

#include <fiber/event/Poller.h>

namespace {

using namespace std::chrono_literals;

struct CountingItem : fiber::event::Poller::Item {
    int calls = 0;
    fiber::event::Poller::Event events = fiber::event::Poller::Event::None;
    CountingItem() { callback = &on_event; }
    static void on_event(fiber::event::Poller::Item *raw, int fd, fiber::event::Poller::Event events) noexcept {
        auto *self = static_cast<CountingItem *>(raw);
        EXPECT_EQ(fd, self->fd());
        ++self->calls;
        self->events = events;
    }
};

int wait_no_intr(fiber::event::Poller &poller, std::chrono::steady_clock::time_point deadline) {
    int count = 0;
    do {
        count = poller.wait(deadline);
    } while (count < 0 && errno == EINTR);
    return count;
}

struct EventFdFixture {
    CountingItem item{};
    int fd = -1;

    ~EventFdFixture() {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool init(fiber::event::Poller &poller) {
        fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        return poller.add(fd, fiber::event::Poller::Event::Read, &item) == fiber::common::IoErr::None;
    }

    bool signal() const {
        const std::uint64_t value = 1;
        return ::write(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
    }

    bool drain() const {
        std::uint64_t value = 0;
        return ::read(fd, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value));
    }
};

} // namespace

TEST(PollerTest, DeadlineReturnsAsTimeout) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, start + 5ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(count, 0);
    EXPECT_GE(elapsed, 2ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(PollerTest, EventReadinessPreemptsInfiniteDeadline) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());

    const int count = wait_no_intr(poller, std::chrono::steady_clock::time_point::max());

    ASSERT_EQ(count, 1);
    EXPECT_EQ(event_fd.item.calls, 0);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 1);
    EXPECT_EQ(event_fd.item.events, fiber::event::Poller::Event::Read);
    EXPECT_TRUE(event_fd.drain());
}

TEST(PollerTest, ExpiredDeadlineDoesNotBlock) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, start - 1ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(count, 0);
    EXPECT_LT(elapsed, 100ms);
}

TEST(PollerTest, EarlierDeadlineRearmsAfterIoWake) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());

    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    ASSERT_TRUE(event_fd.drain());
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, start + 5ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(count, 0);
    EXPECT_GE(elapsed, 2ms);
    EXPECT_LT(elapsed, 100ms);
}

TEST(PollerTest, LaterDeadlineReplacesArmedDeadline) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());

    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 10ms), 1);
    ASSERT_TRUE(event_fd.drain());
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, start + 40ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(count, 0);
    EXPECT_GE(elapsed, 20ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(PollerTest, InfiniteDeadlineDisarmsPreviousTimer) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());

    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 10ms), 1);
    ASSERT_TRUE(event_fd.drain());
    const auto start = std::chrono::steady_clock::now();
    std::atomic<bool> signal_ok{false};
    std::thread waker([&event_fd, &signal_ok]() {
        std::this_thread::sleep_for(30ms);
        signal_ok.store(event_fd.signal(), std::memory_order_release);
    });

    const int count = wait_no_intr(poller, std::chrono::steady_clock::time_point::max());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    waker.join();

    ASSERT_TRUE(signal_ok.load(std::memory_order_acquire));
    ASSERT_EQ(count, 1);
    EXPECT_EQ(event_fd.item.calls, 0);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 1);
    EXPECT_EQ(event_fd.item.events, fiber::event::Poller::Event::Read);
    EXPECT_TRUE(event_fd.drain());
    EXPECT_GE(elapsed, 15ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(PollerTest, DeleteInvalidatesPendingEventAndReAddDeliversOnNextWait) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    ASSERT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
    EXPECT_FALSE(event_fd.item.registered());
    ASSERT_EQ(poller.add(event_fd.fd, fiber::event::Poller::Event::Read, &event_fd.item), fiber::common::IoErr::None);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 0);
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 1);
    EXPECT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
}

TEST(PollerTest, DispatchConsumesBatchOnlyOnce) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    poller.dispatch();
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 1);
    ASSERT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 1);
}

TEST(PollerTest, NewWaitReplacesUndispatchedBatch) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    ASSERT_TRUE(event_fd.drain());
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now()), 0);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 0);
    EXPECT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
}

TEST(PollerTest, FailedAddDoesNotPublishItemAndCanBeRetried) {
    fiber::event::Poller poller;
    CountingItem item;
    EXPECT_NE(poller.add(-1, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    EXPECT_FALSE(item.registered());
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(poller.add(fd, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    EXPECT_TRUE(item.registered());
    EXPECT_EQ(poller.del(item), fiber::common::IoErr::None);
    ::close(fd);
}

TEST(PollerTest, FailedDeleteStillInvalidatesPendingEvent) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    ::close(event_fd.fd);
    event_fd.fd = -1;
    EXPECT_NE(poller.del(event_fd.item), fiber::common::IoErr::None);
    poller.dispatch();
    EXPECT_EQ(event_fd.item.calls, 0);
    EXPECT_FALSE(event_fd.item.registered());
}

TEST(PollerTest, DeleteOfSiblingPreservesOtherPendingEvents) {
    fiber::event::Poller poller;
    EventFdFixture first;
    EventFdFixture second;
    ASSERT_TRUE(first.init(poller));
    ASSERT_TRUE(second.init(poller));
    ASSERT_TRUE(first.signal());
    ASSERT_TRUE(second.signal());
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 2);
    ASSERT_EQ(poller.del(second.item), fiber::common::IoErr::None);
    poller.dispatch();
    EXPECT_EQ(first.item.calls, 1);
    EXPECT_EQ(second.item.calls, 0);
    EXPECT_EQ(poller.del(first.item), fiber::common::IoErr::None);
}

namespace {
struct ReplacingBatch {
    struct Item : fiber::event::Poller::Item {
        Item(ReplacingBatch &context, Callback cb) noexcept : owner(&context) { callback = cb; }
        ReplacingBatch *owner;
    };
    fiber::event::Poller &poller;
    std::optional<Item> items[2];
    int fds[2]{-1, -1};
    int original_calls = 0;
    int replacement_calls = 0;

    ~ReplacingBatch() {
        for (int i = 0; i < 2; ++i) {
            if (items[i] && items[i]->registered()) {
                (void) poller.del(*items[i]);
            }
            if (fds[i] >= 0) {
                ::close(fds[i]);
            }
        }
    }

    static void on_original(fiber::event::Poller::Item *raw, int, fiber::event::Poller::Event) noexcept {
        auto *self = static_cast<Item *>(raw)->owner;
        ++self->original_calls;
        // Replace both self and the pending peer regardless of kernel ordering.
        for (int i = 0; i < 2; ++i) {
            EXPECT_EQ(self->poller.del(*self->items[i]), fiber::common::IoErr::None);
            auto &item = self->items[i].emplace(*self, &on_replacement);
            EXPECT_EQ(self->poller.add(self->fds[i], fiber::event::Poller::Event::Read, &item),
                      fiber::common::IoErr::None);
        }
    }

    static void on_replacement(fiber::event::Poller::Item *raw, int, fiber::event::Poller::Event) noexcept {
        ++static_cast<Item *>(raw)->owner->replacement_calls;
    }
};

struct RecursiveItem : fiber::event::Poller::Item {
    fiber::event::Poller *poller = nullptr;
    bool call_wait = false;
    static void on_event(fiber::event::Poller::Item *raw, int, fiber::event::Poller::Event) noexcept {
        auto *self = static_cast<RecursiveItem *>(raw);
        if (self->call_wait) {
            self->poller->wait(std::chrono::steady_clock::now());
        } else {
            self->poller->dispatch();
        }
    }
};
} // namespace

TEST(PollerTest, CallbackCanReplaceSelfAndPendingPeerAtSameAddresses) {
    fiber::event::Poller poller;
    ReplacingBatch context{poller};
    for (int i = 0; i < 2; ++i) {
        auto &item = context.items[i].emplace(context, &ReplacingBatch::on_original);
        context.fds[i] = ::eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
        ASSERT_GE(context.fds[i], 0);
        ASSERT_EQ(poller.add(context.fds[i], fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    }
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 2);
    poller.dispatch();
    EXPECT_EQ(context.original_calls, 1);
    EXPECT_EQ(context.replacement_calls, 0);
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 2);
    poller.dispatch();
    EXPECT_EQ(context.original_calls, 1);
    EXPECT_EQ(context.replacement_calls, 2);
}

TEST(PollerDeathTest, RejectsWaitAndDispatchDuringCallback) {
    fiber::event::Poller poller;
    RecursiveItem item;
    item.poller = &poller;
    item.callback = &RecursiveItem::on_event;
    const int fd = ::eventfd(1, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(poller.add(fd, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    ASSERT_EQ(wait_no_intr(poller, std::chrono::steady_clock::now() + 1s), 1);
    EXPECT_DEATH(poller.dispatch(), "poller dispatch cannot be recursive");
    item.call_wait = true;
    EXPECT_DEATH(poller.dispatch(), "poller wait cannot run during dispatch");
    EXPECT_EQ(poller.del(item), fiber::common::IoErr::None);
    ::close(fd);
}
