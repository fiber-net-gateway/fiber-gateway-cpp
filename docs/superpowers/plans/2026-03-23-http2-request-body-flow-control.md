# HTTP/2 Request Body Flow Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add HTTP/2 request body buffering and `HttpExchange::read_body()` support with per-stream receive-window backpressure and threshold-based `WINDOW_UPDATE` replenishment.

**Architecture:** Keep protocol-level receive-window accounting in `Http2Connection` and `Http2Stream`, while `ServerHttp2Request` owns the unread request-body queue and `read_body()` semantics. Connection-level flow control is maintained near max for protocol continuity, while stream-level replenishment happens only after the application consumes buffered body and the configured low-watermark conditions are met.

**Tech Stack:** C++23, CMake, GoogleTest, custom `IoBuf`/`IoBufChain`, custom async tasks/event loop

---

### Task 1: Lock Down Body Read Behavior With Failing Tests

**Files:**
- Modify: `tests/Http2ConnectionTest.cpp`
- Modify: `src/http/ServerHttp2Request.h`
- Modify: `src/http/ServerHttp2Request.cpp`

- [ ] **Step 1: Write failing tests for buffered HTTP/2 request body reads**

Add tests that prove:
- a server handler can call `exchange.read_body()` on an HTTP/2 request and receive buffered DATA
- `read_body()` returns `last=true` only after the remote request body is fully complete
- a request body split across multiple DATA frames is returned in order

- [ ] **Step 2: Run the targeted HTTP/2 tests to verify they fail**

Run: `ctest --test-dir build --output-on-failure -R Http2ConnectionTest`
Expected: FAIL in the new HTTP/2 request-body tests because `ServerHttp2Request::read_body()` still returns `NotSupported`

### Task 2: Implement Minimal Buffered `read_body()` Support

**Files:**
- Modify: `src/http/ServerHttp2Request.h`
- Modify: `src/http/ServerHttp2Request.cpp`
- Test: `tests/Http2ConnectionTest.cpp`

- [ ] **Step 1: Add minimal request-body queue state**

Add a queue of unread body buffers, queued-byte tracking, and a single-reader wait state to `ServerHttp2Request`.

- [ ] **Step 2: Make the tests pass with minimal implementation**

Implement:
- `on_body()` appends body buffers into the unread queue and marks end-of-stream
- `read_body()` drains up to `max_bytes` from the queue, waits when empty but not finished, and returns `last=true` once the stream body is complete

- [ ] **Step 3: Run the targeted tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure -R Http2ConnectionTest`
Expected: PASS for the new body-read tests

### Task 3: Lock Down Receive-Window Accounting With Failing Tests

**Files:**
- Modify: `tests/Http2ConnectionTest.cpp`
- Modify: `include/fiber/http/Http2Connection.h`
- Modify: `src/http/Http2Connection.cpp`
- Modify: `include/fiber/http/Http2Stream.h`
- Modify: `src/http/Http2Stream.cpp`

- [ ] **Step 1: Write failing tests for receive-window behavior**

Add tests that prove:
- connection receive window is replenished back to target once it drops below the low watermark
- stream receive window is not replenished when DATA is only buffered
- stream receive window is replenished after `read_body()` consumes enough bytes and both low-watermark conditions are met
- DATA beyond the available stream receive window results in `FLOW_CONTROL_ERROR`

- [ ] **Step 2: Run the targeted tests to verify they fail**

Run: `ctest --test-dir build --output-on-failure -R Http2ConnectionTest`
Expected: FAIL because receive-side flow-control accounting and replenishment are not implemented yet

### Task 4: Implement Receive-Window Accounting And Threshold Replenishment

**Files:**
- Modify: `include/fiber/http/Http2Connection.h`
- Modify: `src/http/Http2Connection.cpp`
- Modify: `include/fiber/http/Http2Stream.h`
- Modify: `src/http/Http2Stream.cpp`
- Modify: `src/http/ServerHttp2Request.h`
- Modify: `src/http/ServerHttp2Request.cpp`
- Test: `tests/Http2ConnectionTest.cpp`

- [ ] **Step 1: Add protocol-level receive-window state**

Add:
- connection receive-window target, low-watermark, and remaining-credit state
- per-stream receive-window remaining-credit state initialized from the configured stream budget

- [ ] **Step 2: Enforce DATA receive-window consumption**

Update `Http2Connection::handle_data_payload()` so each DATA frame:
- computes flow-controlled length
- decrements connection and stream receive windows
- raises `FLOW_CONTROL_ERROR` on underflow before handing body bytes to the request owner

- [ ] **Step 3: Add connection low-watermark replenishment**

When connection receive credit falls below the low watermark, send `WINDOW_UPDATE(stream=0)` back to the configured target.

- [ ] **Step 4: Add stream low-watermark replenishment from `read_body()`**

After `read_body()` consumes bytes:
- compute `remain_body_size`
- if `stream_window_remaining <= stream_low_watermark` and `remain_body_size <= stream_low_watermark`
- send `WINDOW_UPDATE(stream_id, increment)` where `increment = (stream_budget - remain_body_size) - stream_window_remaining`

- [ ] **Step 5: Re-run targeted tests**

Run: `ctest --test-dir build --output-on-failure -R Http2ConnectionTest`
Expected: PASS for the new and existing relevant HTTP/2 tests

### Task 5: Verify Broader Safety

**Files:**
- Modify: `tests/Http2ConnectionTest.cpp`
- Modify: `tests/Http1ServerTest.cpp` (only if any shared exchange behavior regresses)

- [ ] **Step 1: Run the focused HTTP/2 suite**

Run: `ctest --test-dir build --output-on-failure -R Http2`
Expected: PASS

- [ ] **Step 2: Run the full test suite if focused tests are green**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS
