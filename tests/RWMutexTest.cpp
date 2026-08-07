#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <future>
#include <thread>

#include <fiber/async/CoroutinePromiseBase.h>
#include <fiber/async/RWMutex.h>
#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/event/EventLoopGroup.h>

namespace {

using DetachedTask = fiber::async::DetachedTask;

class ManualTask {
public:
    struct promise_type : fiber::async::CoroutinePromiseBase {
        ManualTask get_return_object() { return ManualTask{handle_type::from_promise(*this)}; }

        std::suspend_always initial_suspend() noexcept { return {}; }

        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() noexcept {}

        void unhandled_exception() { std::terminate(); }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit ManualTask(handle_type handle) : handle_(handle) {}

    ManualTask(const ManualTask &) = delete;
    ManualTask &operator=(const ManualTask &) = delete;

    ManualTask(ManualTask &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    ManualTask &operator=(ManualTask &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (handle_) {
            handle_.destroy();
        }
        handle_ = other.handle_;
        other.handle_ = nullptr;
        return *this;
    }

    ~ManualTask() {
        if (handle_) {
            handle_.destroy();
        }
    }

    handle_type release() noexcept {
        handle_type out = handle_;
        handle_ = nullptr;
        return out;
    }

private:
    handle_type handle_{};
};

DetachedTask hold_read_lock(fiber::async::RWMutex *mutex, std::atomic<int> *active_readers,
                            std::promise<int> *second_reader, std::atomic<bool> *reported_second,
                            std::atomic<int> *completed, std::promise<void> *done) {
    auto guard = co_await mutex->lock_shared();
    int current = active_readers->fetch_add(1, std::memory_order_relaxed) + 1;
    if (current == 2 && !reported_second->exchange(true, std::memory_order_relaxed)) {
        second_reader->set_value(current);
    }
    co_await fiber::async::sleep(std::chrono::milliseconds(30));
    active_readers->fetch_sub(1, std::memory_order_relaxed);
    if (completed->fetch_add(1, std::memory_order_relaxed) + 1 == 2) {
        done->set_value();
        fiber::event::EventLoop::current().stop();
    }
    co_return;
}

DetachedTask hold_read_then_write(fiber::async::RWMutex *mutex, std::atomic<int> *state) {
    auto guard = co_await mutex->lock_shared();
    state->store(1, std::memory_order_relaxed);
    co_await fiber::async::sleep(std::chrono::milliseconds(30));
    state->store(2, std::memory_order_relaxed);
    co_return;
}

DetachedTask wait_write_lock(fiber::async::RWMutex *mutex, std::atomic<int> *state, std::promise<int> *promise) {
    auto guard = co_await mutex->lock();
    promise->set_value(state->load(std::memory_order_relaxed));
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask hold_reader_with_signal(fiber::async::RWMutex *mutex, std::promise<void> *locked,
                                     std::chrono::steady_clock::duration delay) {
    auto guard = co_await mutex->lock_shared();
    locked->set_value();
    co_await fiber::async::sleep(delay);
    co_return;
}

DetachedTask wait_writer_order(fiber::async::RWMutex *mutex, std::atomic<int> *order) {
    auto guard = co_await mutex->lock();
    order->store(1, std::memory_order_relaxed);
    co_await fiber::async::sleep(std::chrono::milliseconds(20));
    co_return;
}

DetachedTask wait_reader_order(fiber::async::RWMutex *mutex, std::atomic<int> *order, std::promise<int> *promise) {
    auto guard = co_await mutex->lock_shared();
    promise->set_value(order->fetch_add(1, std::memory_order_relaxed) + 1);
    fiber::event::EventLoop::current().stop();
    co_return;
}

DetachedTask hold_writer_with_signal(fiber::async::RWMutex *mutex, std::promise<void> *locked,
                                     std::chrono::steady_clock::duration delay) {
    auto guard = co_await mutex->lock();
    locked->set_value();
    co_await fiber::async::sleep(delay);
    co_return;
}

DetachedTask wait_read_lock_thread(fiber::async::RWMutex *mutex, std::promise<std::thread::id> *resumed) {
    auto guard = co_await mutex->lock_shared();
    resumed->set_value(std::this_thread::get_id());
    co_return;
}

ManualTask wait_write_then_hit(fiber::async::RWMutex *mutex, std::atomic<int> *hits) {
    auto guard = co_await mutex->lock();
    hits->fetch_add(1, std::memory_order_relaxed);
    co_return;
}

DetachedTask hold_read_and_finish(fiber::async::RWMutex *mutex, std::promise<void> *done) {
    {
        auto guard = co_await mutex->lock_shared();
        co_await fiber::async::sleep(std::chrono::milliseconds(30));
    }
    done->set_value();
    fiber::event::EventLoop::current().stop();
    co_return;
}

} // namespace

TEST(RWMutexTest, ReadersCanShareLock) {
    fiber::event::EventLoopGroup group(1);
    fiber::async::RWMutex mutex;
    std::atomic<int> active_readers{0};
    std::atomic<bool> reported_second{false};
    std::atomic<int> completed{0};
    std::promise<int> promise;
    std::promise<void> done;
    auto future = promise.get_future();
    auto done_future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() {
        return hold_read_lock(&mutex, &active_readers, &promise, &reported_second, &completed, &done);
    });
    fiber::async::spawn(group.at(0), [&]() {
        return hold_read_lock(&mutex, &active_readers, &promise, &reported_second, &completed, &done);
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "shared readers did not overlap in time";
        return;
    }

    EXPECT_EQ(future.get(), 2);
    if (done_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "shared readers did not finish in time";
        return;
    }
    group.join();
}

TEST(RWMutexTest, WriterWaitsForReadersToDrain) {
    fiber::event::EventLoopGroup group(1);
    fiber::async::RWMutex mutex;
    std::atomic<int> state{0};
    std::promise<int> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return hold_read_then_write(&mutex, &state); });
    fiber::async::spawn(group.at(0), [&]() { return wait_write_lock(&mutex, &state, &promise); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "writer did not resume in time";
        return;
    }

    EXPECT_EQ(future.get(), 2);
    group.join();
}

TEST(RWMutexTest, WaitingWriterBlocksLaterReaders) {
    fiber::event::EventLoopGroup group(1);
    fiber::async::RWMutex mutex;
    std::promise<void> locked;
    auto locked_future = locked.get_future();
    std::atomic<int> order{0};
    std::promise<int> promise;
    auto future = promise.get_future();

    group.start();
    fiber::async::spawn(group.at(0),
                        [&]() { return hold_reader_with_signal(&mutex, &locked, std::chrono::milliseconds(50)); });

    if (locked_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "initial reader did not acquire in time";
        return;
    }

    fiber::async::spawn(group.at(0), [&]() { return wait_writer_order(&mutex, &order); });
    fiber::async::spawn(group.at(0), [&]() { return wait_reader_order(&mutex, &order, &promise); });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "later reader did not resume in time";
        return;
    }

    EXPECT_EQ(future.get(), 2);
    group.join();
}

TEST(RWMutexTest, ResumesReaderOnWaiterLoopThread) {
    fiber::event::EventLoopGroup group(2);
    fiber::async::RWMutex mutex;
    std::promise<std::thread::id> loop0_id;
    std::promise<std::thread::id> loop1_id;
    auto loop0_future = loop0_id.get_future();
    auto loop1_future = loop1_id.get_future();
    std::promise<void> locked;
    auto locked_future = locked.get_future();
    std::promise<std::thread::id> resumed;
    auto resumed_future = resumed.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        loop0_id.set_value(std::this_thread::get_id());
        co_return;
    });
    fiber::async::spawn(group.at(1), [&]() -> DetachedTask {
        loop1_id.set_value(std::this_thread::get_id());
        co_return;
    });

    if (loop0_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready ||
        loop1_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "loop threads did not report ids in time";
        return;
    }

    fiber::async::spawn(group.at(0),
                        [&]() { return hold_writer_with_signal(&mutex, &locked, std::chrono::milliseconds(50)); });

    if (locked_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "writer did not acquire in time";
        return;
    }

    fiber::async::spawn(group.at(1), [&]() { return wait_read_lock_thread(&mutex, &resumed); });

    if (resumed_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "reader did not resume in time";
        return;
    }

    EXPECT_EQ(resumed_future.get(), loop1_future.get());
    group.stop();
    group.join();
}

TEST(RWMutexTest, CancelWriteWaiterDoesNotResume) {
    fiber::event::EventLoopGroup group(1);
    fiber::async::RWMutex mutex;
    std::atomic<int> hits{0};
    std::promise<void> done;
    auto future = done.get_future();

    group.start();
    fiber::async::spawn(group.at(0), [&]() { return hold_read_and_finish(&mutex, &done); });
    fiber::async::spawn(group.at(0), [&]() -> DetachedTask {
        auto waiter = wait_write_then_hit(&mutex, &hits);
        auto handle = waiter.release();
        handle.resume();
        handle.destroy();
        co_return;
    });

    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        group.stop();
        group.join();
        FAIL() << "reader holder did not finish in time";
        return;
    }

    EXPECT_EQ(hits.load(std::memory_order_relaxed), 0);
    group.join();
}
