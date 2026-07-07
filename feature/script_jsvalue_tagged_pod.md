# Script `JsValue` Tagged-POD Migration

## Context

The script runtime is moving toward an LLVM-backed native execution path for hot code.
The current `fiber::json::JsValue` shape is not a good fit for that goal:

- it has user-defined copy/move/destroy behavior
- it mixes value representation concerns with ownership semantics
- GC root scanning currently reasons about several value variants directly
- request/response workloads want borrowed string/binary slices, not eager heap copies

For the request flow used by this project:

1. parse JSON from HTTP request
2. run script logic, possibly including service calls
3. build JSON response

`JsValue` should become a small, trivially-copyable tagged value that can live in:

- VM stack slots
- variable tables
- array/object entries
- JIT frames

without destructor traffic or ownership confusion.

## Target Model

The target value model is:

- immediate
  - `Undefined`
  - `Null`
  - `Boolean`
  - `Int64`
  - `Double`
- borrowed
  - borrowed UTF-8 string slice
  - borrowed binary slice
- heap reference
  - `GcString`
  - `GcBinary`
  - `GcArray`
  - `GcObject`
  - `GcException`
  - `GcIterator`

At the end of the migration, `JsValue` should be:

- trivially copyable
- non-owning
- stable in memory layout
- cheap to spill in interpreter and JIT frames
- easy for GC to scan by checking whether the value is a heap reference

## Migration Strategy

### Phase 1: Make `JsValue` trivially copyable without changing runtime semantics

Goals:

- remove custom copy/move/destroy logic
- keep existing field names and most call sites intact
- add helper predicates for heap vs borrowed values

This phase is intentionally conservative. It creates a stable base for the next steps.

### Phase 2: Split borrowed and heap semantics cleanly

Goals:

- treat borrowed string/binary values as data views at the `JsTag` layer
- keep heap string/binary as GC-managed objects
- move all materialization through explicit helpers such as `ensure_heap_string`

Rules:

- object keys should be heap strings only
- value strings may remain borrowed until a mutating or persistent operation requires heap materialization

### Phase 3: GC scans heap refs only

Goals:

- `gc_mark_value` checks only whether a value is a heap ref
- borrowed values are never scanned
- root scanning for interpreter and future JIT frames becomes layout-driven

### Phase 4: Async boundary promotion

Goals:

- borrowed slices that point into request buffers must not outlive the backing storage
- before async suspend, frame values still borrowing request-owned storage must either:
  - be guaranteed pinned for the full script lifetime, or
  - be promoted to heap values

The runtime should provide explicit promotion helpers for this.

### Phase 5: Compact fixed-layout representation

After the semantics settle, the internal layout can be compacted to a fixed tagged POD
intended for both the interpreter and LLVM backend. A likely direction is a fixed-width
16-byte value that stores:

- payload
- length / flags
- tag
- subtag

This phase is intentionally deferred until the borrowed-vs-heap split is complete.

## Runtime Design Rules

- `JsValue` never owns memory directly.
- Borrowed string/binary values point to external storage only.
- GC only manages heap objects.
- Object keys are heap strings, ideally interned later.
- `JsValue` must remain safe to copy with plain assignment and `memcpy`.

## Execution Backend Implications

With this model:

- the interpreter can continue using `JsValue` arrays for stack and vars
- GC can scan frames by testing for heap refs
- a future LLVM backend can use the same frame layout
- numeric paths can inline operations without destructor costs
- borrowed request values can flow through the script without immediate heap allocation

## Implementation Order In This Repository

1. Refactor `src/script/json/JsNode.h` and `src/script/json/JsNode.cpp`
   - make `JsValue` trivially copyable
   - keep compatibility helpers
2. Update `src/script/json/JsGc.cpp`
   - use heap-ref predicates when scanning values
3. Update `src/common/json/JsValueOps.cpp`
   - rely on borrowed-vs-heap helpers instead of ad hoc ownership assumptions
4. Update script runtime entry points
   - `src/script/run/Access.cpp`
   - `src/script/run/InterpreterVm.cpp`
5. Add async-boundary promotion hooks
6. Only then start the LLVM backend work

## Non-Goals For The Initial Refactor

- no full NaN-boxing redesign yet
- no standalone AOT binary generation
- no collector rewrite
- no immediate async semantic changes
