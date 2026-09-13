#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

#include <fiber/event/Poller.h>

namespace {

using namespace std::chrono_literals;

void ignore_event(fiber::event::Poller::Item *, int, fiber::event::Poller::Event) {}

int wait_no_intr(fiber::event::Poller &poller, epoll_event *events, int max_events,
                 std::chrono::steady_clock::time_point deadline) {
    int count = 0;
    do {
        count = poller.wait(events, max_events, deadline);
    } while (count < 0 && errno == EINTR);
    return count;
}

struct EventFdFixture {
    fiber::event::Poller::Item item{};
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
        item.callback = &ignore_event;
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
    epoll_event events[2]{};
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, events, 2, start + 5ms);
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
    epoll_event events[2]{};

    const int count = wait_no_intr(poller, events, 2, std::chrono::steady_clock::time_point::max());

    ASSERT_EQ(count, 1);
    void *event_owner = poller.resolve(events[0].data.u64);
    EXPECT_EQ(event_owner, &event_fd.item);
    EXPECT_TRUE(event_fd.drain());
}

TEST(PollerTest, ExpiredDeadlineDoesNotBlock) {
    fiber::event::Poller poller;
    ASSERT_TRUE(poller.valid());
    epoll_event events[2]{};
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, events, 2, start - 1ms);
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
    epoll_event events[2]{};

    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 1s), 1);
    ASSERT_TRUE(event_fd.drain());
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, events, 2, start + 5ms);
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
    epoll_event events[2]{};

    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 10ms), 1);
    ASSERT_TRUE(event_fd.drain());
    const auto start = std::chrono::steady_clock::now();

    const int count = wait_no_intr(poller, events, 2, start + 40ms);
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
    epoll_event events[2]{};

    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 10ms), 1);
    ASSERT_TRUE(event_fd.drain());
    const auto start = std::chrono::steady_clock::now();
    std::atomic<bool> signal_ok{false};
    std::thread waker([&event_fd, &signal_ok]() {
        std::this_thread::sleep_for(30ms);
        signal_ok.store(event_fd.signal(), std::memory_order_release);
    });

    const int count = wait_no_intr(poller, events, 2, std::chrono::steady_clock::time_point::max());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    waker.join();

    ASSERT_TRUE(signal_ok.load(std::memory_order_acquire));
    ASSERT_EQ(count, 1);
    void *event_owner = poller.resolve(events[0].data.u64);
    EXPECT_EQ(event_owner, &event_fd.item);
    EXPECT_TRUE(event_fd.drain());
    EXPECT_GE(elapsed, 15ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(PollerTest, RemovedTokenCannotResolveAfterSameItemAndFdAreReused) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    epoll_event events[2]{};
    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 1s), 1);
    const auto old_token = events[0].data.u64;
    ASSERT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
    EXPECT_EQ(poller.resolve(old_token), nullptr);
    ASSERT_EQ(poller.add(event_fd.fd, fiber::event::Poller::Event::Read, &event_fd.item), fiber::common::IoErr::None);
    EXPECT_NE(event_fd.item.token(), old_token);
    EXPECT_EQ(poller.resolve(old_token), nullptr);
    EXPECT_EQ(poller.resolve(event_fd.item.token()), &event_fd.item);
    EXPECT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
}

TEST(PollerTest, FailedAddDoesNotPublishItemAndCanBeRetried) {
    fiber::event::Poller poller;
    fiber::event::Poller::Item item;
    EXPECT_NE(poller.add(-1, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    EXPECT_EQ(item.token(), 0U);
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(poller.add(fd, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    EXPECT_EQ(poller.resolve(item.token()), &item);
    EXPECT_EQ(poller.del(item), fiber::common::IoErr::None);
    ::close(fd);
}

TEST(PollerTest, FailedDeleteStillInvalidatesReturnedEvent) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    const auto token = event_fd.item.token();
    ::close(event_fd.fd);
    event_fd.fd = -1;
    EXPECT_NE(poller.del(event_fd.item), fiber::common::IoErr::None);
    EXPECT_EQ(poller.resolve(token), nullptr);
    EXPECT_EQ(event_fd.item.token(), 0U);
}

TEST(PollerTest, RegistryGrowthPreservesEarlierTokens) {
    fiber::event::Poller poller;
    EventFdFixture sockets[130];
    for (auto &socket: sockets) {
        ASSERT_TRUE(socket.init(poller));
    }
    for (auto &socket: sockets) {
        EXPECT_EQ(poller.resolve(socket.item.token()), &socket.item);
        EXPECT_EQ(poller.del(socket.item), fiber::common::IoErr::None);
    }
}
