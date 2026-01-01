# Script Interpreter Migration Plan (Java -> C++)

## Goals
- Port the Java script interpreter (fiber-gateway-script) to C++ in this repo.
- Keep language syntax and semantics aligned with doc/user.md (Java version).
- Use C++20 coroutine for async execution.
- Interpreter-only (bytecode VM), no AOT/JIT in the first phase.
- Represent `missing` as `Undefined` (JsNodeType::Undefined).

## Scope and Constraints
- Grammar: limited JS-like subset from doc/user.md.
- Data model: reuse `JsValue`, `GcHeap`, `GcString`, `GcArray`, `GcObject`.
- Errors: equivalent to `ScriptExecException`, include position info.
- Encoding: strings already handled by `JsValueOps` and `GcString`.

## Architecture Overview
1) Frontend: Tokenizer -> Parser -> AST
2) Middle: optional AST optimization (constant folding, dead-code checks)
3) Backend: AST -> bytecode compiler -> VM interpreter
4) Runtime: value ops + access ops + standard library + async bridge

## Language Support Matrix
- Statements: let, if/else, for-of, continue, break, return, try/catch, throw, directive.
- Expressions: unary (+, -, !, typeof, ...), binary (+, -, *, /, %, ~, &&, ||, <, <=, >, >=, ==, ===, !=, !==, in), ternary (?:).
- Literals: numbers, strings, arrays, objects, null, true/false.
- Access: obj.prop, obj[key], array index, function calls, spread in arrays/objects.

## Module Layout (C++)
- `src/script/parse/TokenKind.h|.cpp`
- `src/script/parse/Tokenizer.h|.cpp`
- `src/script/parse/Parser.h|.cpp`
- `src/script/ast/*`
- `src/script/parse/Optimiser.h|.cpp`
- `src/script/ir/Code.h|.cpp`
- `src/script/ir/Compiler.h|.cpp`
- `src/script/ir/Compiled.h|.cpp`
- `src/script/run/InterpreterVm.h|.cpp`
- `src/script/run/Access.h|.cpp`
- `src/script/run/Compares.h|.cpp`
- `src/script/run/Unaries.h|.cpp`
- `src/script/run/Binaries.h|.cpp`
- `src/script/Script.h|.cpp`
- `src/script/Library.h|.cpp`
- `src/script/ExecutionContext.h|.cpp`
- `src/script/std/*`
- `tests/Script*Test.cpp`

## Runtime Model
- `missing` is represented as `JsNodeType::Undefined`.
- `null` is `JsNodeType::Null`.
- boolean/number/string/object/array/binary align with `JsValue`.
- `Iterator` and `Exception` map to `GcIterator` and `GcException`.

## Bytecode Design
- Mirror Java `Code` opcodes to reduce semantic drift:
  - LOAD_CONST, LOAD_ROOT, LOAD_VAR, STORE_VAR, DUMP, POP
  - NEW_OBJECT, NEW_ARRAY, EXP_OBJECT, EXP_ARRAY, PUSH_ARRAY
  - IDX_GET, IDX_SET, IDX_SET_1, PROP_GET, PROP_SET, PROP_SET_1
  - BOP_*, UNARY_*
  - CALL_FUNC, CALL_FUNC_SPREAD
  - CALL_ASYNC_FUNC, CALL_ASYNC_FUNC_SPREAD
  - CALL_CONST, CALL_ASYNC_CONST
  - JUMP, JUMP_IF_FALSE, JUMP_IF_TRUE
  - ITERATE_INTO, ITERATE_NEXT, ITERATE_KEY, ITERATE_VALUE
  - INTO_CATCH, THROW_EXP, END_RETURN
- `Compiled` contains: codes, operands, positions, exception table, stack size, var size.

## Parser/Compiler Alignment Notes
- Tokenizer must implement `===`, `!==`, `...`, `&&`, `||`, `in` and all operators.
- Parser should follow Java precedence and statement parsing order.
- Compiler must respect variable scoping, shadowing, and jump patching (break/continue/try).

## Coroutine Task and Scheduler Design (C++20)
### Task Model
- Use a small `Task<T>` coroutine type (lazy by default).
- `Task<T>` holds `std::expected<T, TaskError>` and resumes a stored continuation.
- `Task<T>::operator co_await()` returns an awaiter that:
  - if ready, returns immediately.
  - otherwise stores the awaiting coroutine handle as continuation.
- Provide `Task<void>` specialization.

### Scheduler Interface
```
struct IScheduler {
    virtual void post(std::coroutine_handle<> h) = 0;
    virtual ~IScheduler() = default;
};

struct InlineScheduler final : IScheduler {
    void post(std::coroutine_handle<> h) override { h.resume(); }
};
```
- `TaskPromiseBase` stores `IScheduler* scheduler` (nullable).  
- When a task completes, it resumes continuation via `scheduler->post(handle)` if set, otherwise `handle.resume()` inline.

### Bridge for Callback-Style Async
```
template <typename T>
class TaskCompletionSource {
public:
    Task<T> task();
    void set_value(T value);
    void set_error(TaskError error);
};
```
- Allows integration with existing async APIs without coroutines.
- `task()` returns a `Task<T>` that completes when `set_value` or `set_error` is called.

### ExecutionContext and Library Async APIs
- `ExecutionContext` exposes `root()`, `attach()`, args, and `scheduler()` access.
- Sync functions: `JsValue Function::call(ExecutionContext&)`.
- Async functions: `Task<JsValue> AsyncFunction::call(ExecutionContext&)`.
- Async constants: `Task<JsValue> AsyncConstant::get(ExecutionContext&)`.

### VM Integration
- `InterpreterVm::exec_async()` is a coroutine returning `Task<JsValue>`.
- When executing `CALL_ASYNC_FUNC`/`CALL_ASYNC_CONST`, VM does:
  - `auto val = co_await async_func.call(ctx);`
  - push `val` and continue.
- `pc/sp` are stored in the VM instance; coroutine suspension keeps them intact.
- A sync API `exec_sync()` rejects bytecode that contains async opcodes.

### Lifetime and Ownership
- `InterpreterVm` is created on heap and owned by the coroutine frame.
- `Script::exec_async()` creates the VM and returns a `Task<JsValue>` that keeps it alive.
- Sync execution uses a stack VM instance with no coroutine suspension.

## InterpreterVm Design (Detailed)
### Responsibilities
- Execute bytecode from `ir::Compiled` with Java-aligned opcodes.
- Provide `ExecutionContext` for library calls.
- Handle errors via `std::expected` and propagate positions.
- Integrate with GC by exposing VM roots at collection time (single-coroutine execution).

### Core State
- `const ir::Compiled &compiled_`
- `fiber::json::GcHeap *heap_` (provided by execution entry)
- `fiber::json::JsValue root_`, `void *attach_`, `async::IScheduler *scheduler_`
- `std::vector<fiber::json::JsValue> stack_`, `std::vector<fiber::json::JsValue> vars_`
- `std::size_t sp_`, `std::size_t pc_`
- Argument view: `std::size_t arg_off_`, `std::size_t arg_cnt_`, optional `GcArray *spread_args_`
- Const cache: `std::vector<fiber::json::JsValue> const_cache_` (same size as operands)
- Error state: `VmError pending_error_`, `bool has_error_`

### Execution APIs
- `exec_sync()` returns `std::expected<JsValue, VmError>`
  - Rejects async opcodes if present.
- `exec_async()` returns `async::Task<std::expected<JsValue, VmError>>`
  - Suspends only at `CALL_ASYNC_*`.

### Opcode Execution
- Stack machine, 32-bit instruction with low 8-bit opcode and upper bits as operands.
- `LOAD_CONST` uses heap-safe constants:
  - Numeric/bool/null/undefined -> direct value.
  - String/binary -> allocate via `GcHeap` and cache in `const_cache_`.
  - No `GcHeap` stored inside `Compiled`.
- `CALL_FUNC`/`CALL_CONST` use `arg_off_/arg_cnt_` to expose args in `ExecutionContext`.
- `CALL_ASYNC_*` does `co_await` and resumes with same `pc_/sp_`.
- `JUMP_IF_*` uses `Compares::logic`.
- `END_RETURN` produces final value (or `Undefined` if empty stack).

### Error Model (No Exceptions)
- Ops return `std::expected<JsValue, VmError>` or `bool` + error out param.
- VM converts error to `pending_error_` and enters `catch_for_exception`.
- `THROW_EXP` converts value -> `VmError` (or `GcException`) then routes through catch.
- All error paths attach position from `compiled_.positions[pc - 1]`.

### Try/Catch Table
- `compiled_.exception_table` is converted to `exp_ins` (same layout as Java).
- `search_catch(pc)` uses linear/binary search for catch target.
- `INTO_CATCH` writes error object to a variable slot for catch block.

### GC Integration (Scan VM Directly)
- Single-coroutine execution allows GC at safe points (allocation sites or between opcodes).
- `GcRootSet` owns globals/temps and a list of `RootProvider` instances.
- `InterpreterVm` implements `RootProvider::visit_roots` and is registered for the VM lifetime.
- Roots exposed by VM:
  - `root_`
  - `stack_[0..sp_)`
  - `vars_`
  - `args_`/`spread_args_`
  - `const_cache_`
  - `pending_error_` and any pending return value
- GC does not require per-push updates; it scans VM state only when collecting.
- Temporary values not on the VM stack can use `GcRootHandle` sparingly.

## GcRootSet + RootProvider Design
### Interfaces
```
struct RootVisitor {
    void visit(fiber::json::JsValue *value);
    void visit_range(fiber::json::JsValue *base, std::size_t count);
};

class RootProvider {
public:
    virtual ~RootProvider() = default;
    virtual void visit_roots(RootVisitor &visitor) = 0;
};

class GcRootSet {
public:
    void add_global(fiber::json::JsValue *value);
    void remove_global(fiber::json::JsValue *value);
    void add_temp_root(fiber::json::JsValue *value);
    void remove_temp_root(fiber::json::JsValue *value);
    void add_provider(RootProvider *provider);
    void remove_provider(RootProvider *provider);
    void visit_all(RootVisitor &visitor);
};
```

### Root Scanning Order
- `globals_` -> `temps_` -> `providers_` (VMs and other runtime scopes).
- `RootVisitor` internally stores a temporary root vector or calls `gc_mark_value` directly.

### InterpreterVm RootProvider
- `visit_roots` should include:
  - `root_`
  - `stack_[0..sp_)` using `visit_range`
  - `vars_` using `visit_range`
  - `args_` if stored separately (or `stack_` slice if args are a view)
  - `spread_args_` (if not null)
  - `const_cache_` (non-undefined entries)
  - `pending_error_` and `pending_return_` (if present)

### GC Trigger Points
- Triggered only at safe points and by the runtime that owns both heap + roots:
  - allocation wrappers check `heap.bytes` vs `threshold` and call `gc_collect(heap, rootset)`
  - between opcode dispatch iterations
  - before/after async suspension
- `GcHeap` does not hold a rootset and does not auto-collect; collection is always requested by runtime.
- No per-push/per-pop root registration.
- The VM must be in a consistent state (no partially updated stack) at collection.

### Ops GC Safety Rules
- Default rule: ops (`Access`/`Unaries`/`Binaries`/`Compares`) may call `runtime->maybe_collect()` only at entry.
- If an op must allocate multiple objects and may need GC mid-op, it must protect all temporary `JsValue` with temp-root guards.
- Never do "allocate -> collect -> retry" inside ops unless every intermediate value is already rooted.

## Test Plan (Java Parity)
### Harness Assumptions
Tests execute via Script (parse → compile → VM). Standard library packages (`array`, `Object`, `strings`, `binary`, `hash`, `JSON`, `math`, `rand`, `time`, `url`) are registered in the runtime. Host objects used by examples (e.g. `req`, directive packages) are stubbed in tests with deterministic behavior.

### Core Language Coverage
- Literals + typeof: verify number/string/boolean/null/missing/object/array/binary typing.
```javascript
let num = 1;
let txt = "this is string";
let bin = req.readBinary();
let boo = true;
let nul = null;
let obj = {n:num};
let mis = obj.cc;
let arr = [1,2,num];
let result = {num, txt, bin, nul, obj, boo, mis, arr};
let types = {};
for (let k, v of result) { types[k] = typeof v; }
return {types, result};
```
Expect: types map matches `number/string/binary/boolean/null/object/missing/array`.

- Arithmetic + precedence: numeric operators and precedence.
```javascript
return 1 + 2 * 3 - 4 / 2 + (5 % 2);
```
Expect: result is `1 + 6 - 2 + 1 = 6`.

- String concat and coercion (number + string).
```javascript
return 1 + "a" + 2;
```
Expect: `"1a2"`.

- Logical && and || short-circuit.
```javascript
let v = 0;
let a = v && (v = 2);
let b = v || (v = 3);
return {a, b, v};
```
Expect: `{a:0, b:3, v:3}` (right side of `&&` not evaluated).

- Comparisons + equality (`==` vs `===`, `!=` vs `!==`).
```javascript
return {
  a: 1 == "1",
  b: 1 === "1",
  c: 1 != "1",
  d: 1 !== "1"
};
```
Expect: `{a:true, b:false, c:false, d:true}`.

- `in` operator with object.
```javascript
let obj = {n:1};
return {t: "n" in obj, f: "x" in obj};
```
Expect: `{t:true, f:false}`.

- Unary ops: `+`, `-`, `!`, `typeof`.
```javascript
return {a:+("3"), b:-(2), c:!0, d:typeof null};
```
Expect: `{a:3, b:-2, c:true, d:"null"}`.

- Ternary.
```javascript
return (1 > 2) ? "no" : "yes";
```
Expect: `"yes"`.

- Array/object literals + access + assignment.
```javascript
let o = {a:1};
let a = [o.a, 2];
o.a = 3;
a[1] = 4;
return {o, a};
```
Expect: `{o:{a:3}, a:[1,4]}`.

- Spread in array/object and call.
```javascript
let a = [1,2];
let b = [0, ...a, 3];
let o = {a:1};
let p = {z:0, ...o, b:2};
return {b, p, sum: add(...b)};
```
Expect: `b` = `[0,1,2,3]`, `p` has keys `z,a,b`, `sum` equals `6` (host `add` stub).

- if/else + return.
```javascript
let v = 2;
if (v > 1) { return "big"; }
return "small";
```
Expect: `"big"`.

- for-of over array with break/continue.
```javascript
let arr = [10, 20, 30];
let out = [];
for (let i, v of arr) {
  if (i == 0) { continue; }
  out.push(v);
  break;
}
return out;
```
Expect: `[20]`.

- for-of over object keys/values.
```javascript
let obj = {a:1, b:2};
let out = {};
for (let k, v of obj) { out[k] = v + 1; }
return out;
```
Expect: `{a:2, b:3}` (iteration order aligned with Java impl).

- try/catch + throw (string and object).
```javascript
try { throw "err"; } catch (e) { return e; }
```
Expect: `"err"`.

```javascript
let obj = {a:1};
try { throw obj; } catch (e) { return e === obj; }
```
Expect: `true`.

- Directive statement + package call (stubbed package).
```javascript
directive demoService from dubbo "com.test.dubbo.DemoService";
return demoService.createUser("name");
```
Expect: matches stubbed package return.

### Standard Library Coverage
- length/includes.
```javascript
return {
  a: length("abc") === 3,
  b: length({a:1,b:2}) === 2,
  c: length([1,2,3]) === 3,
  d: length(1) === 0,
  e: includes("abcabc", "cab") === true,
  f: includes(["aa","bb","cc"], "aa") === true,
  g: includes({a:1}, "a") === false
};
```
Expect: all fields true.

- array.push/pop/join.
```javascript
let a = [1,2];
let b = array.push(a, 3, 4);
let c = array.pop(a);
return {same: a === b, c, join: array.join(a, "-"), len: length(a)};
```
Expect: `{same:true, c:4, join:"1-2-3", len:3}`.

- Object.assign/keys/values/deleteProperties.
```javascript
let a = {a:1,b:2};
Object.assign(a, {c:3});
let keys = Object.keys(a);
let values = Object.values(a);
Object.deleteProperties(a, "a", "x");
return {len:length(a), a:a.a, keys, values};
```
Expect: `len` is 2, `a` is missing, `keys` and `values` include `b,c`.

- strings.* core set.
```javascript
return {
  prefix: strings.hasPrefix("abcdedf", "abc"),
  suffix: strings.hasSuffix("abcdedf", "edf"),
  lower: strings.toLower("AbC") === "abc",
  upper: strings.toUpper("AbC") === "ABC",
  trim: strings.trim("  \tabc\t ") === "abc",
  split: strings.split("abcecdf", "c")[1] === "e",
  contains: strings.contains("abcd-effe-ssf-fd", "e-ssf"),
  index: strings.index("aabbcc", "bcc") === 3,
  last: strings.lastIndex("cabcd", "c") === 3,
  repeat: strings.repeat("acd", 3) === "acdacdacd",
  match: strings.match("aaabbbbccc", "a+b+c+"),
  substring: strings.substring("0123456789", 3, 6) === "345"
};
```
Expect: all fields true.

- binary.* + hash.* (stable vectors).
```javascript
let bin = binary.base64Decode("AQID");
return {
  b64: binary.base64Encode(bin) === "AQID",
  hex: binary.hex(bin) === "010203",
  crc: hash.crc32("abc") === 891568578,
  md5: hash.md5("abc") === "900150983cd24fb0d6963f7d28e17f72",
  sha1: hash.sha1("abc") === "a9993e364706816aba3e25717850c26c9cd0d89d",
  sha256: hash.sha256("abc") === "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
};
```
Expect: all fields true.

- JSON.parse/stringify.
```javascript
let obj = JSON.parse("{\"a\":1,\"b\":[2,3]}");
return JSON.stringify(obj) === "{\"a\":1,\"b\":[2,3]}";
```
Expect: `true`.

- math.*.
```javascript
return {a: math.floor(3.9) === 3, b: math.abs(-4) === 4};
```
Expect: all fields true.

- rand.* (deterministic stub for tests).
```javascript
return {a: rand.canary("42") === 42, b: rand.random() >= 0};
```
Expect: `a` true, `b` true (random stubbed).

- time.* (fixed clock stub).
```javascript
return time.format(1700000000, "yyyy-MM-dd") === "2023-11-14";
```
Expect: `true` for fixed clock.

- url.*.
```javascript
let q = url.parseQuery("a=1&b=2");
return url.buildQuery(q) === "a=1&b=2"
  && url.encodeComponent("a b") === "a%20b"
  && url.decodeComponent("a%20b") === "a b";
```
Expect: `true`.

### Error and Edge Coverage
- Missing property yields `missing` type.
```javascript
let o = {};
return typeof o.miss;
```
Expect: `"missing"`.

- Invalid operand or builtin type mismatch throws (positioned error).
```javascript
array.push(1, 2);
```
Expect: throws error with accurate position.

- Syntax errors include position.
```javascript
let a = [1, 2;
```
Expect: parse error with line/column.

## GcHeap / GcRootSet / Runtime Interfaces
### GcHeap (memory only, no root ownership)
```
struct GcHeap {
    std::size_t bytes = 0;
    std::size_t threshold = 1 << 20;
};

std::size_t gc_bytes_used(const GcHeap &heap);
std::size_t gc_threshold(const GcHeap &heap);
void gc_set_threshold(GcHeap &heap, std::size_t value);
void gc_collect(GcHeap &heap, GcRootSet &roots);
```
- `gc_collect` is invoked by runtime when a threshold is exceeded or after an allocation failure.

### GcRootSet (root aggregation only)
```
class GcRootSet {
public:
    void add_global(fiber::json::JsValue *value);
    void remove_global(fiber::json::JsValue *value);
    void add_temp_root(fiber::json::JsValue *value);
    void remove_temp_root(fiber::json::JsValue *value);
    void add_provider(RootProvider *provider);
    void remove_provider(RootProvider *provider);
    void visit_all(RootVisitor &visitor);
};
```
- `GcRootSet` does not own `GcHeap`; it only aggregates roots and providers.

### Runtime (heap + roots owner, GC trigger)
```
class ScriptRuntime {
public:
    ScriptRuntime(fiber::json::GcHeap &heap, fiber::json::GcRootSet &roots);

    fiber::json::GcHeap &heap();
    fiber::json::GcRootSet &roots();

    bool should_collect(std::size_t next_bytes = 0) const;
    void maybe_collect(std::size_t next_bytes = 0);

    template <typename AllocFn>
    auto alloc_with_gc(std::size_t next_bytes, AllocFn &&fn) -> decltype(fn());
};
```
- `alloc_with_gc` performs: `if (should_collect(next_bytes)) gc_collect(heap_, roots_);` then runs `fn()`.
- If `fn()` fails due to memory pressure, runtime may collect once and retry.

### ScriptRuntime Temp Root Guards
```
class GcRootGuard {
public:
    GcRootGuard(ScriptRuntime &runtime, fiber::json::JsValue *value);
    GcRootGuard(const GcRootGuard &) = delete;
    GcRootGuard &operator=(const GcRootGuard &) = delete;
    ~GcRootGuard();
private:
    fiber::json::GcRootSet *roots_ = nullptr;
    fiber::json::JsValue *value_ = nullptr;
};

class TempRootScope {
public:
    explicit TempRootScope(ScriptRuntime &runtime);
    void add(fiber::json::JsValue *value);
private:
    fiber::json::GcRootSet *roots_ = nullptr;
};
```
- Use `GcRootGuard` for single temporary values in ops.
- Use `TempRootScope` when multiple temporaries are created before they reach VM stack/vars.

## VmError and Error Object Conversion
### VmError Shape
```
struct VmError {
    std::string name;
    std::string message;
    int status = 500;
    std::int64_t position = -1;
    fiber::json::JsValue meta; // optional, may be Undefined
};
```

### From Runtime Error to VmError
- Arithmetic/type errors from ops produce `VmError` with:
  - `name = "EXEC_COMPUTE_ERROR"` or specific operator name
  - `message` matching Java `SpelMessage` text
  - `status = 500`
  - `position = compiled_.positions[pc - 1]`
- Unsupported operator/type combinations set `name = "EXEC_UNSUPPORTED_OP"`.

### From `throw` Value to VmError
- If value is `GcException`: extract `name/message/status/meta/position`.
- If value is object:
  - read `"name"`, `"message"`, `"status"`, `"meta"` (if present)
  - fall back to defaults if missing
- Otherwise:
  - `name = "EXEC_THROW_ERROR"`, `message = "execute script throw error"`, `status = 500`

### VmError to Error Object (for `catch`)
- Convert to `GcException` via `gc_new_exception` using `name/message/position/meta`.
- `INTO_CATCH` stores this exception object (or object form if required by API).

### Error Propagation
- Any op returning `std::unexpected(VmError)` sets `pending_error_` and triggers `catch_for_exception`.
- `THROW_EXP` directly builds `VmError` from the thrown value then goes through the same path.
- If no catch is found, `exec_*` returns `std::unexpected(VmError)`.

## Standard Library Plan
Phase 1:
- length, includes
- array: push, pop, join
- object: assign, keys
- string/json/math/binary/hash/rand/time/url as needed

Phase 2:
- directive binding and host integration
- additional utility functions

## Testing Plan
- Port Java tests and script resources to gtest.
- Coverage targets:
  - operators (including ===/!==)
  - for-of, break/continue
  - try/catch/throw
  - missing vs null behavior
  - standard library functions
  - async calls (basic success/error cases)

## Implementation Sequence
1) Tokenizer + TokenKind
2) Parser + AST
3) Compiler -> bytecode (no async)
4) Interpreter VM (sync)
5) Ops: Access/Compares/Unaries/Binaries alignment
6) Async via coroutine + async opcodes
7) Std library
8) Tests and parity checks

## Open Questions (to confirm before coding)
- Directive resolution interface details in C++.
- Whether to support regex `~` matching in phase 1.
