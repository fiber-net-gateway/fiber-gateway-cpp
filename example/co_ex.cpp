//
// Created by pc on 4/23/26.
//

#include <chrono>
#include <print>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include <fiber/async/Sleep.h>
#include <fiber/async/Spawn.h>
#include <fiber/async/Task.h>
#include <fiber/common/IoError.h>
#include <fiber/event/EventLoop.h>

fiber::async::DetachedTask f() {

    auto p = []() -> fiber::async::Task<void> {
        std::println(("============== 1 ========"));
        co_await fiber::async::sleep(std::chrono::milliseconds(1000));
        std::println(("============== 2 ========"));
        co_await fiber::async::sleep(std::chrono::milliseconds(1000));
        std::println(("============== 3 ========"));
    };

    co_await p();
    std::println(("============== -- ========"));
    co_await p();
}

int main() {

    fiber::event::EventLoop loop;

    fiber::async::spawn(loop, f);

    loop.run();

    return 0;
}
