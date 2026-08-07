#ifndef FIBER_ASYNC_WHEN_ANY_H
#define FIBER_ASYNC_WHEN_ANY_H

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "../common/Assert.h"
#include "Awaitable.h"

namespace fiber::async {

namespace detail {

template<std::size_t Index, typename Result>
struct WhenAnyAlternative {
    using Value = std::remove_cvref_t<Result>;

    template<typename U>
        requires std::constructible_from<Value, U>
    explicit WhenAnyAlternative(U &&value) noexcept(std::is_nothrow_constructible_v<Value, U>) :
        value(std::forward<U>(value)) {}

    Value value;
};

template<std::size_t Index, typename T>
struct WhenAnyAlternative<Index, T &> {
    explicit WhenAnyAlternative(T &value) noexcept : value(value) {}

    std::reference_wrapper<T> value;
};

template<std::size_t Index>
struct WhenAnyAlternative<Index, void> {};

struct WhenAnyFromFactories {};

template<std::size_t Index, SelectableAwaiter Awaiter>
class AwaiterSlot {
protected:
    template<typename Factory>
    explicit AwaiterSlot(WhenAnyFromFactories tag, Factory &factory) : storage_(tag, factory) {}

public:
    ~AwaiterSlot() noexcept { reset(); }

    Awaiter &awaiter() noexcept {
        FIBER_ASSERT(active_);
        return storage_.awaiter;
    }

    const Awaiter &awaiter() const noexcept {
        FIBER_ASSERT(active_);
        return storage_.awaiter;
    }

    void reset() noexcept {
        if (!active_) {
            return;
        }
        storage_.awaiter.~Awaiter();
        active_ = false;
    }

private:
    union Storage {
        template<typename Factory>
        explicit Storage(WhenAnyFromFactories, Factory &factory) : awaiter(std::invoke(factory)) {}

        ~Storage() noexcept {}

        Awaiter awaiter;
    } storage_;

    bool active_ = true;
};

template<typename IndexSequence, typename... Awaiters>
class AwaiterStorage;

template<std::size_t... Indexes, typename... Awaiters>
class AwaiterStorage<std::index_sequence<Indexes...>, Awaiters...> : private AwaiterSlot<Indexes, Awaiters>... {
protected:
    template<typename... Factories>
        requires(sizeof...(Factories) == sizeof...(Awaiters))
    explicit AwaiterStorage(WhenAnyFromFactories tag, Factories &...factories) :
        AwaiterSlot<Indexes, Awaiters>(tag, factories)... {}

    template<std::size_t Index>
    auto &awaiter() noexcept {
        using Awaiter = std::tuple_element_t<Index, std::tuple<Awaiters...>>;
        return static_cast<AwaiterSlot<Index, Awaiter> &>(*this).awaiter();
    }

    template<std::size_t Index>
    const auto &awaiter() const noexcept {
        using Awaiter = std::tuple_element_t<Index, std::tuple<Awaiters...>>;
        return static_cast<const AwaiterSlot<Index, Awaiter> &>(*this).awaiter();
    }

    template<std::size_t Index>
    void destroy() noexcept {
        using Awaiter = std::tuple_element_t<Index, std::tuple<Awaiters...>>;
        static_cast<AwaiterSlot<Index, Awaiter> &>(*this).reset();
    }
};

} // namespace detail

template<typename... Results>
class [[nodiscard]] WhenAnyResult {
private:
    template<std::size_t... Indexes>
    static auto variant_type(std::index_sequence<Indexes...>)
            -> std::variant<detail::WhenAnyAlternative<Indexes, Results>...>;

    using Variant = decltype(variant_type(std::index_sequence_for<Results...>{}));

public:
    template<std::size_t Index>
    explicit WhenAnyResult(std::in_place_index_t<Index>) : value_(std::in_place_index<Index>) {}

    template<std::size_t Index, typename U>
    explicit WhenAnyResult(std::in_place_index_t<Index>, U &&value) :
        value_(std::in_place_index<Index>, std::forward<U>(value)) {}

    [[nodiscard]] std::size_t index() const noexcept { return value_.index(); }

    template<std::size_t Index>
    [[nodiscard]] bool is() const noexcept {
        static_assert(Index < sizeof...(Results));
        return value_.index() == Index;
    }

    template<std::size_t Index>
    decltype(auto) get() & {
        static_assert(Index < sizeof...(Results));
        auto *alternative = std::get_if<Index>(&value_);
        FIBER_ASSERT(alternative != nullptr);
        using Result = std::tuple_element_t<Index, std::tuple<Results...>>;
        if constexpr (std::is_void_v<Result>) {
            return;
        } else if constexpr (std::is_lvalue_reference_v<Result>) {
            return alternative->value.get();
        } else {
            return (alternative->value);
        }
    }

    template<std::size_t Index>
    decltype(auto) get() const & {
        static_assert(Index < sizeof...(Results));
        const auto *alternative = std::get_if<Index>(&value_);
        FIBER_ASSERT(alternative != nullptr);
        using Result = std::tuple_element_t<Index, std::tuple<Results...>>;
        if constexpr (std::is_void_v<Result>) {
            return;
        } else if constexpr (std::is_lvalue_reference_v<Result>) {
            return alternative->value.get();
        } else {
            return (alternative->value);
        }
    }

    template<std::size_t Index>
    decltype(auto) get() && {
        static_assert(Index < sizeof...(Results));
        auto *alternative = std::get_if<Index>(&value_);
        FIBER_ASSERT(alternative != nullptr);
        using Result = std::tuple_element_t<Index, std::tuple<Results...>>;
        if constexpr (std::is_void_v<Result>) {
            return;
        } else if constexpr (std::is_lvalue_reference_v<Result>) {
            return alternative->value.get();
        } else {
            return std::move(alternative->value);
        }
    }

private:
    Variant value_;
};

template<typename... Awaiters>
    requires(sizeof...(Awaiters) >= 2 && (SelectableAwaiter<Awaiters> && ...))
class [[nodiscard]] WhenAnyAwaiter final
    : private detail::AwaiterStorage<std::index_sequence_for<Awaiters...>, Awaiters...> {
private:
    using Storage = detail::AwaiterStorage<std::index_sequence_for<Awaiters...>, Awaiters...>;
    static constexpr std::size_t kAwaiterCount = sizeof...(Awaiters);
    static constexpr std::size_t kNoWinner = kAwaiterCount;

    enum class Phase : std::uint8_t { Created, NotReady, Arming, Suspended, Ready, Resumed };

public:
    using Result = WhenAnyResult<decltype(std::declval<Awaiters &>().await_resume())...>;

    template<typename... Factories>
        requires(sizeof...(Factories) == sizeof...(Awaiters))
    explicit WhenAnyAwaiter(detail::WhenAnyFromFactories tag, Factories &...factories) : Storage(tag, factories...) {}

    WhenAnyAwaiter(const WhenAnyAwaiter &) = delete;
    WhenAnyAwaiter &operator=(const WhenAnyAwaiter &) = delete;
    WhenAnyAwaiter(WhenAnyAwaiter &&) = delete;
    WhenAnyAwaiter &operator=(WhenAnyAwaiter &&) = delete;
    ~WhenAnyAwaiter() = default;

    bool await_ready() {
        FIBER_ASSERT(phase_ == Phase::Created);
        if (find_ready<0>()) {
            phase_ = Phase::Ready;
            return true;
        }
        phase_ = Phase::NotReady;
        return false;
    }

    bool await_suspend(std::coroutine_handle<> parent) {
        FIBER_ASSERT(phase_ == Phase::NotReady);
        phase_ = Phase::Arming;
        if (!suspend_from<0>(parent)) {
            phase_ = Phase::Ready;
            return false;
        }
        phase_ = Phase::Suspended;
        return true;
    }

    Result await_resume() {
        FIBER_ASSERT(phase_ == Phase::Ready || phase_ == Phase::Suspended);
        if (winner_ == kNoWinner) {
            find_async_winner<0>();
            FIBER_ASSERT(winner_ != kNoWinner);
        }
        destroy_losers<0>();
        phase_ = Phase::Resumed;
        return resume_winner<0>();
    }

    [[nodiscard]] bool completed() const noexcept {
        if (winner_ != kNoWinner || phase_ == Phase::Resumed) {
            return true;
        }
        return any_completed<0>();
    }

private:
    template<std::size_t Index>
    bool find_ready() {
        if constexpr (Index == kAwaiterCount) {
            return false;
        } else {
            auto &awaiter = Storage::template awaiter<Index>();
            if (awaiter.await_ready()) {
                FIBER_ASSERT(awaiter.completed());
                winner_ = Index;
                return true;
            }
            FIBER_ASSERT(!awaiter.completed());
            return find_ready<Index + 1>();
        }
    }

    template<std::size_t Index>
    bool suspend_from(std::coroutine_handle<> parent) {
        if constexpr (Index == kAwaiterCount) {
            return true;
        } else {
            auto &awaiter = Storage::template awaiter<Index>();
            using SuspendResult = detail::AwaitSuspendResult<std::remove_reference_t<decltype(awaiter)>>;
            if constexpr (std::same_as<SuspendResult, void>) {
                awaiter.await_suspend(parent);
            } else {
                if (!awaiter.await_suspend(parent)) {
                    FIBER_ASSERT(awaiter.completed());
                    winner_ = Index;
                    return false;
                }
            }
            FIBER_ASSERT(!awaiter.completed());
            return suspend_from<Index + 1>(parent);
        }
    }

    template<std::size_t Index>
    void find_async_winner() noexcept {
        if constexpr (Index < kAwaiterCount) {
            const auto &awaiter = Storage::template awaiter<Index>();
            if (awaiter.completed()) {
                FIBER_ASSERT(winner_ == kNoWinner);
                winner_ = Index;
            }
            find_async_winner<Index + 1>();
        }
    }

    template<std::size_t Index>
    [[nodiscard]] bool any_completed() const noexcept {
        if constexpr (Index == kAwaiterCount) {
            return false;
        } else {
            return Storage::template awaiter<Index>().completed() || any_completed<Index + 1>();
        }
    }

    template<std::size_t Index>
    void destroy_losers() noexcept {
        if constexpr (Index < kAwaiterCount) {
            if (winner_ != Index) {
                Storage::template destroy<Index>();
            }
            destroy_losers<Index + 1>();
        }
    }

    template<std::size_t Index>
    Result resume_winner() {
        if constexpr (Index == kAwaiterCount) {
            FIBER_PANIC("WhenAnyAwaiter winner index is invalid");
        } else {
            if (winner_ != Index) {
                return resume_winner<Index + 1>();
            }
            auto &awaiter = Storage::template awaiter<Index>();
            using InnerResult = decltype(awaiter.await_resume());
            if constexpr (std::is_void_v<InnerResult>) {
                awaiter.await_resume();
                return Result(std::in_place_index<Index>);
            } else {
                return Result(std::in_place_index<Index>, awaiter.await_resume());
            }
        }
    }

    std::size_t winner_ = kNoWinner;
    Phase phase_ = Phase::Created;
};

template<typename... Factories>
    requires(sizeof...(Factories) >= 2 && (SelectableAwaiterFactory<Factories> && ...))
[[nodiscard]] auto when_any(Factories &&...factories) {
    using Awaiter = WhenAnyAwaiter<std::remove_cvref_t<std::invoke_result_t<Factories &>>...>;
    return Awaiter(detail::WhenAnyFromFactories{}, factories...);
}

} // namespace fiber::async

#endif // FIBER_ASYNC_WHEN_ANY_H
