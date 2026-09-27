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

// SelectableAwaiter is the entry ticket to when_any / timeout_for wrappers,
// and it carries the library's cancellation model as a behavioral contract.
// Cancellation here is frame destruction, never a generic in-flight cancel():
// a Task is an opaque coroutine tree with no reflection to its innermost
// suspended awaiter, so when_any discards a loser and timeout_for abandons a
// timed-out operation by destroying the suspended coroutine frame, which runs
// every in-flight awaiter's destructor. Destruction-Cancels rules, required of
// every SelectableAwaiter whenever it is destroyed while suspended:
//   (i)   de-register everything it registered on its owning loop -- timers,
//         poller subscriptions, waiter queues, kernel state;
//   (ii)  never resume the awaiting coroutine from the destructor;
//   (iii) if a resume is already queued, it must be retractable before the
//         awaiter storage goes away (WaitAwaiter's cancellable local defer
//         queue is the reference pattern; an MPSC entry cannot be retracted);
//   (iv)  nothrow, as the concept already demands.
// The compiler can only check the signature parts; rules (i)-(iii) are upheld
// by the destruction-safety regression suites.
template<typename T>
concept SelectableAwaiter =
        detail::Awaiter<T> && std::is_nothrow_destructible_v<T> &&
        (std::same_as<detail::AwaitSuspendResult<T>, void> || std::same_as<detail::AwaitSuspendResult<T>, bool>) &&
        requires(const T &awaiter) {
            { awaiter.completed() } noexcept -> std::same_as<bool>;
        };

// A leaf awaiter fit for *external* active cancellation only: a party other
// than the awaiting coroutine calls cancel() while the awaiter is suspended.
// Contract: cancel() MUST resume the awaiting coroutine exactly once, with a
// terminal error -- "may resume" is not acceptable, callers could not write
// correct code against it. timeout_for / when_any must never invoke it before
// their own resume (for a resuming cancel() that is a double resume); they
// cancel through destruction instead. Note that CancellableAwaiter does NOT
// imply SelectableAwaiter: ConnectAwaiter satisfies this concept (external
// cancellation drives it in DnsClient) but has no completed(), while
// Watch::NextAwaiter's queue retraction is a destructor-only helper that does
// not resume and therefore must stay out of this concept.
template<typename T>
concept CancellableAwaiter = detail::Awaiter<T> && std::is_nothrow_destructible_v<T> && requires(T &awaiter) {
    { awaiter.cancel() } noexcept;
};

template<typename Factory>
concept SelectableAwaiterFactory = std::invocable<Factory &> && !std::is_reference_v<std::invoke_result_t<Factory &>> &&
                                   SelectableAwaiter<std::remove_cvref_t<std::invoke_result_t<Factory &>>>;

} // namespace fiber::async

#endif // FIBER_ASYNC_AWAITABLE_H
