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
    const void *event_ptr = events[0].data.ptr; // epoll_event is packed: read via a local
    EXPECT_EQ(event_ptr, &event_fd.item);
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
    const void *event_ptr = events[0].data.ptr; // epoll_event is packed: read via a local
    EXPECT_EQ(event_ptr, &event_fd.item);
    EXPECT_TRUE(event_fd.drain());
    EXPECT_GE(elapsed, 15ms);
    EXPECT_LT(elapsed, 1s);
}

TEST(PollerTest, DeleteBlanksReturnedEventAndReAddDeliversAgain) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    epoll_event events[2]{};
    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 1s), 1);
    const void *event_ptr = events[0].data.ptr; // epoll_event is packed: read via a local
    ASSERT_EQ(event_ptr, &event_fd.item);
    ASSERT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
    event_ptr = events[0].data.ptr;
    EXPECT_EQ(event_ptr, nullptr);
    EXPECT_FALSE(event_fd.item.registered());
    ASSERT_EQ(poller.add(event_fd.fd, fiber::event::Poller::Event::Read, &event_fd.item), fiber::common::IoErr::None);
    EXPECT_TRUE(event_fd.item.registered());
    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 1s), 1);
    event_ptr = events[0].data.ptr;
    EXPECT_EQ(event_ptr, &event_fd.item);
    EXPECT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
}

TEST(PollerTest, DeleteOutsideBatchDoesNotTouchStaleArray) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    epoll_event events[2]{};
    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 1s), 1);
    poller.end_batch();
    ASSERT_EQ(poller.del(event_fd.item), fiber::common::IoErr::None);
    const void *event_ptr = events[0].data.ptr; // epoll_event is packed: read via a local
    EXPECT_EQ(event_ptr, &event_fd.item);
}

TEST(PollerTest, FailedAddDoesNotPublishItemAndCanBeRetried) {
    fiber::event::Poller poller;
    fiber::event::Poller::Item item;
    EXPECT_NE(poller.add(-1, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    EXPECT_FALSE(item.registered());
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(poller.add(fd, fiber::event::Poller::Event::Read, &item), fiber::common::IoErr::None);
    EXPECT_TRUE(item.registered());
    EXPECT_EQ(poller.del(item), fiber::common::IoErr::None);
    ::close(fd);
}

TEST(PollerTest, FailedDeleteStillInvalidatesReturnedEvent) {
    fiber::event::Poller poller;
    EventFdFixture event_fd;
    ASSERT_TRUE(event_fd.init(poller));
    ASSERT_TRUE(event_fd.signal());
    epoll_event events[2]{};
    ASSERT_EQ(wait_no_intr(poller, events, 2, std::chrono::steady_clock::now() + 1s), 1);
    ::close(event_fd.fd);
    event_fd.fd = -1;
    EXPECT_NE(poller.del(event_fd.item), fiber::common::IoErr::None);
    const void *event_ptr = events[0].data.ptr; // epoll_event is packed: read via a local
    EXPECT_EQ(event_ptr, nullptr);
    EXPECT_FALSE(event_fd.item.registered());
}

TEST(PollerTest, DeleteOfSiblingBlanksOnlyItsEntries) {
    fiber::event::Poller poller;
    EventFdFixture first;
    EventFdFixture second;
    ASSERT_TRUE(first.init(poller));
    ASSERT_TRUE(second.init(poller));
    ASSERT_TRUE(first.signal());
    ASSERT_TRUE(second.signal());
    epoll_event events[4]{};
    ASSERT_EQ(wait_no_intr(poller, events, 4, std::chrono::steady_clock::now() + 1s), 2);
    ASSERT_EQ(poller.del(second.item), fiber::common::IoErr::None);
    int survivors = 0;
    for (int i = 0; i < 2; ++i) {
        // epoll_event is packed (align 4): read data.ptr through a local so the
        // comparison never binds a reference to the misaligned member.
        const void *event_ptr = events[i].data.ptr;
        EXPECT_NE(event_ptr, &second.item);
        if (event_ptr == &first.item) {
            ++survivors;
        }
    }
    EXPECT_EQ(survivors, 1);
    EXPECT_EQ(poller.del(first.item), fiber::common::IoErr::None);
}
