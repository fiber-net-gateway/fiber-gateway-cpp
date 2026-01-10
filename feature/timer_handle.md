# EventLoop Timer Handle API (Draft)

## Goals
- Avoid `TimerEntry* -> Handle*` casts in user callbacks.
- Keep zero-allocation timer usage with minimal per-timer overhead.
- Support fixed (compile-time) callback functions without storing a callback
  pointer inside `Handle`.
- Preserve existing loop-thread-only semantics for timer operations.

## Non-Goals
- Per-instance callback selection (that would require storing a callback in
  `Handle` or `TimerEntry`).
- Cross-thread `post_at` or `cancel` semantics changes.
- Automatic lifetime management for `Handle`.

## Data Model
`EventLoop::TimerEntry` gains an extra field:
- `std::ptrdiff_t handle_offset`:
  - Set by `post_at` at schedule time.
  - Records the byte offset from `Handle` to its `TimerEntry` subobject.

The existing `TimerEntry::callback` remains `void (*)(TimerEntry*)`, but it is
always set to a template-generated trampoline that restores `Handle*` and calls
the fixed callback `Cb(Handle*)`.

## Concepts (C++23)
These concepts are used to enforce correct template usage at compile time.

```cpp
#include <concepts>
#include <type_traits>

namespace fiber::event::detail {

template <typename Handle, typename Entry, auto EntryMember>
concept TimerEntryMember =
    std::is_object_v<Handle> &&
    !std::is_const_v<Handle> &&
    std::same_as<decltype(EntryMember), Entry Handle::*> &&
    requires(Handle& h) {
        { h.*EntryMember } -> std::same_as<Entry&>;
    };

template <typename Handle, auto Cb>
concept TimerCallback =
    std::same_as<decltype(Cb), void (*)(Handle*)>;

} // namespace fiber::event::detail
```

## API Draft
Template overloads that use a fixed callback function pointer:

```cpp
namespace fiber::event {

class EventLoop {
public:
    template <typename Handle,
              auto EntryMember,
              auto Cb>
    requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember> &&
             detail::TimerCallback<Handle, Cb>
    void post_at(std::chrono::steady_clock::time_point when, Handle& handle);

    template <typename Handle,
              auto EntryMember>
    requires detail::TimerEntryMember<Handle, TimerEntry, EntryMember>
    void cancel(Handle& handle);
};

} // namespace fiber::event
```

The existing `post_at(time_point, TimerEntry&)` and `cancel(TimerEntry&)` remain
for compatibility and for cases that do not fit the handle-based API.

## Trampoline Mechanics (Pseudo)
```cpp
template <typename Handle, auto EntryMember, auto Cb>
static void timer_trampoline(EventLoop::TimerEntry* entry) {
    auto* bytes = reinterpret_cast<char*>(entry);
    auto* handle = reinterpret_cast<Handle*>(bytes - entry->handle_offset);
    Cb(handle);
}
```

## Invariants and Safety Rules
- `Handle` must outlive any scheduled timer using its `TimerEntry`.
- `Handle` must not move while its `TimerEntry` is in the heap.
- `EntryMember` must refer to a non-static data member of `Handle`.
- `post_at` and `cancel` are called on the loop thread (same as current API).
- One `TimerEntry` must not be scheduled in the heap more than once at a time.

## Usage Sketch
```cpp
struct MySleep {
    fiber::event::EventLoop::TimerEntry timer{};
    static void on_timer(MySleep* self);
};

// Scheduling:
loop.post_at<MySleep, &MySleep::timer, &MySleep::on_timer>(deadline, obj);
```

## Migration Notes
Existing code that subclasses `TimerEntry` (e.g., `SleepTimer : TimerEntry`)
can either:
- Keep using `post_at(time_point, TimerEntry&)`, or
- Switch to a handle-based wrapper where `TimerEntry` is a member.
