#ifndef FIBER_ASYNC_AWAITABLE_H
#define FIBER_ASYNC_AWAITABLE_H

#include <concepts>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <utility>

namespace fiber::async {

namespace detail {

template<typename T>
concept HasMemberCoAwait = requires(T value) { value.operator co_await(); };

template<typename T>
concept HasFreeCoAwait = requires(T value) { operator co_await(value); };

template<typename T>
decltype(auto) get_awaiter(T &&value) {
    if constexpr (HasMemberCoAwait<T>) {
        return std::forward<T>(value).operator co_await();
    } else if constexpr (HasFreeCoAwait<T>) {
        return operator co_await(std::forward<T>(value));
    } else {
        return std::forward<T>(value);
    }
}

template<typename T>
using AwaiterType = std::remove_cvref_t<decltype(get_awaiter(std::declval<T>()))>;

template<typename T>
concept Awaiter = requires(T awaiter, std::coroutine_handle<> handle) {
    { awaiter.await_ready() } -> std::convertible_to<bool>;
    awaiter.await_suspend(handle);
    awaiter.await_resume();
};

template<typename T>
concept Awaitable = Awaiter<AwaiterType<T>>;

template<typename T>
using AwaitSuspendResult = decltype(std::declval<T &>().await_suspend(std::declval<std::coroutine_handle<>>()));

} // namespace detail

template<typename T>
concept SelectableAwaiter =
        detail::Awaiter<T> && std::is_nothrow_destructible_v<T> &&
        (std::same_as<detail::AwaitSuspendResult<T>, void> || std::same_as<detail::AwaitSuspendResult<T>, bool>) &&
        requires(const T &awaiter) {
            { awaiter.completed() } noexcept -> std::same_as<bool>;
        };

template<typename Factory>
concept SelectableAwaiterFactory = std::invocable<Factory &> && !std::is_reference_v<std::invoke_result_t<Factory &>> &&
                                   SelectableAwaiter<std::remove_cvref_t<std::invoke_result_t<Factory &>>>;

} // namespace fiber::async

#endif // FIBER_ASYNC_AWAITABLE_H
