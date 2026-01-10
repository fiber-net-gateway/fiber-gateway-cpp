# Coroutine Mutex (Awaitable, Multi-Thread Safe)

## Goals
- Provide a coroutine-friendly mutex with `co_await` support.
- Keep operations non-blocking (suspend/resume only).
- Best-effort FIFO fairness for waiters.
- Resume a waiting coroutine on the same event-loop thread it awaited from.
- Forbid cross-thread `unlock()` strictly.

## Core Invariants
- `unlock()` must run on `owner_thread_` (assert otherwise).
- Each waiter captures `EventLoop*` + `thread_id` at `await_suspend`.
- Resumption is always scheduled via `waiter.loop->post(...)`.
- Queue is only mutated under `state_mu_`.

## State Schema
```
Mutex
  state_mu_: std::mutex
  locked_: bool
  owner_thread_: std::thread::id
  waiters_: list<WaiterPtr>

Waiter
  mutex: Mutex*
  handle: std::coroutine_handle<>
  loop: EventLoop*
  thread: std::thread::id
  state: atomic<WaiterState> (Waiting | Notified | Resumed | Canceled)
  queued: bool
```

## State Diagram (Logical)
```
States:
  U = Unlocked
  L0 = Locked, no waiters
  L1 = Locked, waiters present

Transitions:
  lock() success: U  -> L0
  lock() queued:  L0 -> L1, L1 -> L1
  unlock() no waiters: L0 -> U
  unlock() with waiters: L1 -> L0 (last waiter) or L1 -> L1
```

## Queueing Strategy (Best-Effort FIFO)
- Waiters are enqueued in a FIFO list.
- Cancellation removes a waiter from the list.
- Unlock skips canceled waiters.
- No strict fairness beyond FIFO order when waiters remain valid.

## Resume Mechanics
- `unlock()` selects the next waiter under `state_mu_`.
- Ownership transfers immediately: `owner_thread_ = waiter.thread`.
- The waiter is resumed via `waiter.loop->post(...)` to ensure same-thread resume.
- Resumption uses a state transition (`Notified -> Resumed`) to avoid double resume.

## Cancellation & Races
- Cancel while `Waiting`:
  - Remove from queue, mark `Canceled`, clear handle.
- Cancel while `Notified` (resume posted but not run yet):
  - Mark `Canceled`, clear handle.
  - Transfer ownership to next waiter (or unlock if none).
- Cancel while `Resumed`:
  - No mutex action.

## Threading Rules
- Waiters must be created on an event-loop thread (`EventLoop::current_or_null()` non-null).
- `unlock()` asserts `current_thread == owner_thread_`.
- Any thread may enqueue a waiter; resume always returns to the waiter’s loop thread.
