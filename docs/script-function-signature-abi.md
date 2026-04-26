# Script Function Signature ABI

This note defines the ABI and rollout plan for script library functions that are
registered with parameter-count signatures, trailing defaults, and variadic
tails.

## Runtime ABI

Host functions receive a flattened argument array through `Library::HostCallFrame`.

```cpp
struct HostCallFrame {
    ScriptRuntime *runtime;
    const JsValue *root;
    void *attach;
    const JsValue *args;
    std::uint32_t argc;
    std::uint32_t flags;
};
```

The caller guarantees that `args[0..argc)` is the final argument list after:

- overload resolution,
- trailing default argument insertion,
- spread argument expansion.

When `argc == 0`, `args` may be `nullptr`. Host functions must not depend on
whether a value came from an explicit argument, a default argument, or a spread
argument.

For `array.push(arr, ...items)`, the host function receives:

```text
args[0]      = arr
args[1..argc) = items
```

For `foo(obj, b = 1)`, a call to `foo(obj)` reaches the host as:

```text
argc = 2
args[0] = obj
args[1] = 1
```

Parameter-count validation is a parser/compiler responsibility. Host functions
remain responsible for type validation and state validation.

## Signature Model

Each registered function has a signature:

```cpp
struct FunctionSignature {
    std::uint16_t required_argc;
    std::uint16_t fixed_argc;
    bool variadic;
    const JsValue *defaults;
    std::uint16_t default_count;
};
```

The rules are:

- `required_argc <= fixed_argc`.
- Defaults cover only the final `default_count` fixed parameters.
- `default_count == fixed_argc - required_argc`.
- First implementation does not allow defaults and variadic tails on the same
  signature.
- A non-variadic signature matches `required_argc <= argc <= fixed_argc`.
- A variadic signature matches `argc >= fixed_argc`.

Examples:

```text
array.push(arr, ...items)
required_argc = 1
fixed_argc    = 1
variadic      = true

foo(obj, b = 1)
required_argc = 1
fixed_argc    = 2
variadic      = false
default_count = 1
```

## Matching Rules

The parser resolves a function call against the library before the AST is handed
to the IR compiler.

- Unknown function name: compile failure, `function not defined`.
- Known function with no matching arity: compile failure,
  `function argument count mismatch`.
- More than one matching signature: compile failure,
  `ambiguous function call`.
- Unknown-length spread calls may only match variadic signatures.
- For unknown-length spread calls, only arguments before the first spread count
  as statically known fixed arguments. For example, `array.push(arr, ...xs)`
  can match `array.push(arr, ...items)`, while `array.push(...xs)` cannot prove
  that the required `arr` argument exists.

Registration rejects overlapping signatures for the same function name where
one call shape could match more than one entry.

## Rollout Plan

1. Add signature, match request, and match result types to `Library`.
2. Remove name-only `find_func(name)` and `find_async_func(name)` lookup hooks.
3. Store `StdLibrary` functions as per-name entry lists with signatures.
4. Resolve parser function calls by argument shape instead of name only.
5. Store matched defaults on `ast::FunctionCall`.
6. Make the IR compiler append default constants before emitting the call site.
7. Migrate standard library functions from legacy registration to explicit
   signatures and add focused compiler/execution tests.
