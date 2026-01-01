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
- `Task<T>` holds result or `std::exception_ptr` and resumes a stored continuation.
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
    void set_exception(std::exception_ptr error);
};
```
- Allows integration with existing async APIs without coroutines.
- `task()` returns a `Task<T>` that completes when `set_value` or `set_exception` is called.

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
