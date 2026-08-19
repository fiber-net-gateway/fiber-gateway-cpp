# Script Module Guide

English | [简体中文](script-guide.zh-CN.md)

This guide documents the embedded scripting module in `fiber_lib`: the language, standard library, C++ embedding API, HTTP request/response bindings, route constants, and directive-bound upstream HTTP API.

TypeScript declarations in this document describe parameter and return types only. The runtime is not a TypeScript or JavaScript engine and does not read `.d.ts` files. The contracts below follow the current implementations in `src/script/` and `src/http_script/`.

## 1. Overview

The module is a small JS-like bytecode interpreter intended for gateway configuration and request handling:

- `src/script/parse/`: lexer, parser, and template-string parser.
- `src/script/ir/`: AST-to-bytecode compiler.
- `src/script/run/`: synchronous and asynchronous bytecode interpreters.
- `src/script/gc/`: GC heap and script strings, binary values, arrays, and objects.
- `src/script/std/`: the default standard library.
- `src/http_script/`: `HttpExchange` bindings exposed as `req.*`, `resp.*`, request constants, and upstream HTTP directives.

Compilation and execution are separate. Compile once while loading configuration, then execute the compiled `Script` for each request. Each execution needs its own `GcHeap`. A `Script` contains read-only compiled state and may be shared when the host also makes its attached function state safe for the chosen concurrency model.

There is no `await` syntax. The `Library` marks a host function as asynchronous at compile time, but scripts call it like a normal function:

```javascript
let body = req.readJson();
resp.sendJson(200, body);
```

A script containing an asynchronous call must be run with C++ `Script::exec_async()`.

## 2. Quick start

A regular script can read the root value supplied by the host as `$`, compute a value, and return it:

```javascript
let total = 0;
for (let index, item of $.items) {
    total = total + item;
}

return {
    name: $.name,
    total,
    message: `hello ${$.name}`
};
```

An HTTP script usually sends its response directly:

```javascript
let query = req.getQuery();

resp.setHeader("X-Handled-By", "fiber-script");
resp.sendJson(200, {
    method: req.getMethod(),
    path: req.getPath(),
    page: query.page
});
```

## 3. Value types

The script value model can be approximated with these TypeScript types:

```typescript
declare class Binary {
    private readonly __scriptBinaryBrand: never;
}

type ScriptPrimitive = undefined | null | boolean | number | string | Binary;
type ScriptArray = ScriptValue[];
type ScriptObject = { [key: string]: ScriptValue };
type ScriptValue = ScriptPrimitive | ScriptArray | ScriptObject;

type JsonValue = null | boolean | number | string | JsonValue[] | {
    [key: string]: JsonValue;
};
```

Important differences from JavaScript are:

- Numbers are represented internally as signed 64-bit integers or `double`, although both appear as `number` in script declarations.
- `undefined` represents a missing property, an uninitialized variable, or a no-value API result. It is distinct from `null`.
- Strings use WTF-8 storage with UTF-16 semantics. Positions returned by `length()`, string indexing, and `strings.substring()` are UTF-16 code units, not UTF-8 bytes or Unicode code points.
- `Binary` holds raw bytes. There is no binary literal; values normally come from `req.readBinary()`, `binary.*`, or the C++ host.
- Arrays and objects are mutable reference values. `array.push()`, `array.pop()`, `Object.assign()`, and `Object.deleteProperties()` mutate their input.
- `typeof` returns one of `"undefined"`, `"null"`, `"boolean"`, `"number"`, `"string"`, `"binary"`, `"array"`, `"object"`, `"iterator"`, or `"exception"`.

### 3.1 Truthiness

`if`, `!`, `&&`, `||`, and the conditional expression use the same rules:

| Value | Truthy? |
| --- | --- |
| `undefined`, `null` | No |
| `false` | No |
| `true` | Yes |
| Integer `0`, floating-point `0.0`, `-0.0`, and `NaN` | No |
| Every other number, including either infinity | Yes |
| Empty string | No |
| Non-empty string | Yes |
| `Binary`, array, object, iterator, or exception | Yes, including empty containers |

There is no general conversion function for booleans. Use `!!value` when an explicit boolean result is needed.

## 4. Complete language syntax

The language resembles JavaScript but is not an ECMAScript implementation. Do not rely on JavaScript behavior that is not listed here.

### 4.1 Whitespace, comments, identifiers, and semicolons

ASCII spaces, tabs, CR, and LF are ignored. A standalone U+2028/U+2029 in source is not general whitespace. Both comment forms are supported:

```javascript
// A line comment
/* A non-nesting block comment */
```

Use `[A-Za-z_$][A-Za-z0-9_$]*` for portable identifiers. `let`, `if`, `else`, `for`, `of`, `continue`, `break`, `return`, `directive`, `try`, `catch`, and `throw` are keywords. `true`, `false`, `null`, `typeof`, and `in` also have special meanings in their grammar positions and should not be used as names.

Semicolon handling is not JavaScript ASI:

- `let` and `directive` declarations require a trailing semicolon.
- Semicolons after expressions, `return`, `throw`, `break`, and `continue` are optional but recommended.
- Extra empty statements (`;`) are ignored.
- A line break does not terminate `return`; `return\nvalue;` still returns `value`.
- An empty source file is a compile error, while an empty `{}` block is valid.

### 4.2 Scalar literals

```javascript
let decimal = 42;
let longSuffix = 42L;
let hexadecimal = 0x2a;
let real = 3.5;
let leadingDot = .5;
let trailingDot = 1.;
let exponent = 1.25e2;
let floatSuffix = 1.5f;
let doubleSuffix = 1d;
let enabled = true;
let disabled = false;
let empty = null;
```

- Decimal and hexadecimal integers become signed 64-bit integers. `L`/`l` changes lexical classification only; it does not create another runtime type.
- Decimal fractions, exponent notation, and `F`/`f`/`D`/`d` suffixes become `double`. A negative value is a unary `-` expression, not part of the literal.
- Binary literals, numeric separators, BigInt, `NaN`/`Infinity` literals, and a dedicated `undefined` literal are not supported.
- An integer outside `int64_t`, or a real literal outside the parser's range, is a compile error.

### 4.3 Strings and escapes

Single- and double-quoted strings are equivalent:

```javascript
let a = "line\ntext";
let b = 'quote: \'';
let c = "\x41\u4e2d";
```

Supported escapes are `\\`, `\'`, `\"`, `` \` ``, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, `\xHH`, `\uHHHH`, and one to three octal digits. A backslash immediately followed by a line terminator continues the line without adding a character. For compatibility, `\a` currently produces the ordinary character `a`, not a bell. Unescaped newlines and `\u{...}` are not supported in ordinary strings.

Positions count UTF-16 code units: `"😀".length` is 2, and `"😀"[0]` and `"😀"[1]` each return a one-surrogate string. WTF-8 storage preserves those isolated surrogates.

### 4.4 Variables, scopes, and the root value

```javascript
let value;
value = 1;

{
    let value = 2; // Shadows the outer variable.
}

return $.request.id;
```

- `let name;` initializes `name` to `undefined`.
- A script, explicit block, `if`/`else` block, loop, and `catch` block each establish a scope. Inner declarations may shadow outer variables.
- Repeating `let` in the same scope is allowed: it reuses the same slot and the later declaration assigns it again.
- Reading an undeclared ordinary identifier silently creates an implicit `undefined` slot instead of throwing `ReferenceError`; assigning it is also valid. Production scripts should still declare variables explicitly.
- The `root` argument supplied to `exec_sync()`/`exec_async()` is available as `$`. `$.user.id` is regular property access. `$query.name` is a host-resolved constant and is unrelated to the root object.

### 4.5 Array and object literals

```javascript
let name = "fiber";
let base = {a: 1};
let list = [0, 1, 2,];
let object = {
    name,                    // Shorthand property
    "static-key": 1,        // String key
    ["dynamic-" + name]: 2, // Computed key
    ...base,                 // Object spread
};
```

Arrays and objects allow trailing commas. Array holes are not supported. A static object key must be an identifier or string; duplicate static keys in one literal are compile errors. A computed key must evaluate to a string or it throws `TypeError`. Multiple computed keys may overwrite the same property.

Spread behavior is:

```javascript
let list = [0, ...[1, 2], 3];
let values = [...{a: 1, b: 2}]; // [1, 2], in property insertion order
let object = {a: 1, ...{a: 2, b: 3}};
```

- Array spread accepts an array or object. An object contributes its values, not its keys, in insertion order. Other types throw `TypeError`.
- Object spread accepts only an object and copies properties in insertion order. Other types throw `TypeError`.
- Literals and spread create a new outer container but shallow-copy their values.
- A later property overwrites an earlier value without changing the original insertion position.

### 4.6 Member access, indexing, and assignment

```javascript
let first = list[0];
let name = object.name;
let dynamic = object["dynamic-key"];

object.name = "gateway";
object["new-key"] = 2;
list[0] = 9;
```

Read rules:

- Dot access on an object is equivalent to indexing with a string key. A missing property returns `undefined`. Chained reads through `null`, `undefined`, or another scalar also return `undefined` rather than throwing.
- An array index must be a non-negative integer in range; otherwise the result is `undefined`. Floating-point `0.0` is not an array index.
- A string index must be an integer and returns the corresponding UTF-16 code-unit string. An out-of-range index returns `undefined`.
- Dot-property access on an array or string returns its length. The documented spelling is `.length`; the current implementation returns the length for any dot-property name, and scripts must not rely on that compatibility quirk.

Write rules:

- Variables, object properties, and array/object indexes may be assignment targets. Assignment returns the right-hand value and is right-associative, so `a = b = 1` is valid.
- An object index must be a string. Dot assignment and string-index assignment can create a property; failure throws `TypeError`.
- Array assignment accepts only an existing integer index. A negative or out-of-range index throws `RangeError`; a non-integer key throws `TypeError`. Use `array.push()` to append.
- Assigning a property or index on a string, `null`, `undefined`, or another scalar throws `TypeError`.
- `compile_script(..., allow_assign=false)` rejects every assignment expression during parsing.

### 4.7 Function calls and argument spread

Calls use statically resolved, fully qualified names:

```javascript
array.push(items, 1, 2);
array.push(items, ...moreItems);
Object.assign(target, ...sources);
```

- `array.push(a, x)` is a registered name. `a.push(x)` is not a dynamic method call.
- Functions are not script values: they cannot be stored, passed as callbacks, or called dynamically.
- Arguments evaluate left to right. Argument spread accepts an array or object; an object contributes property values in insertion order. Other types throw `TypeError`.
- A call containing spread has an unknown compile-time arity, so it can match only a variadic signature. A fixed-arity function does not match even when the runtime spread length would be correct.
- An unknown function, arity mismatch, synchronous/asynchronous overload conflict, or ambiguous overload is a compile error.
- Asynchronous functions still use normal call syntax. The compiler emits an asynchronous opcode, and the script must be executed with `exec_async()`.

### 4.8 Operator precedence

From highest to lowest:

| Precedence | Operators | Associativity |
| --- | --- | --- |
| 1 | `()`, call, `.`, `[]` | Left-to-right |
| 2 | Unary `+`, `-`, `!`, `typeof` | Right-to-left |
| 3 | `*`, `/`, `%` | Left-to-right |
| 4 | `+`, `-` | Left-to-right |
| 5 | `<`, `<=`, `>`, `>=`, `==`, `!=`, `===`, `!==`, `in`, `~` | Left-to-right |
| 6 | `&&` | Left-to-right, short-circuiting |
| 7 | `||` | Left-to-right, short-circuiting |
| 8 | `?:`, `=` | Right-to-left |

All relational and equality operators share one precedence level, unlike JavaScript. Parenthesize mixed comparisons. The lexer/parser can recognize `~`, but the compiler rejects it; see section 4.17.

### 4.9 Arithmetic and unary operators

Numeric operations accept integers, floating-point numbers, booleans, and `null`; booleans become 0/1 and `null` becomes 0. Ordinary strings are not implicitly converted to numbers.

- Unary `+` returns the numeric conversion; unary `-` negates it. Incompatible types throw `TypeError`. Negating `INT64_MIN` promotes the result to floating point.
- If either operand of `+` is a string, the operator concatenates. The other side may be `undefined`, `null`, boolean, number, or string and uses its compatibility text. Arrays, objects, and `Binary` cause `TypeError`.
- Without a string operand, `+`, `-`, and `*` are numeric. Integer overflow promotes the result to `double`; any floating-point operand also produces floating point.
- `/` always returns floating point. An integer or floating-point zero divisor throws `RangeError`.
- `%` returns an integer remainder for two integers and a floating-point remainder otherwise. A zero divisor throws `RangeError`.
- `!value` always returns a boolean using section 3.1. `typeof value` always returns one of the type strings listed in section 3.

These operators have no side effects apart from allocating a result when required.

### 4.10 Relational, equality, logical, and conditional operators

- `<`, `<=`, `>`, `>=`: two strings compare lexicographically by UTF-16 code unit; two numeric-compatible values convert to `double` and compare numerically. Every other combination, including a string/number mix, returns `false` without throwing.
- `===`, `!==`: all numbers compare after conversion to `double`, so integer/floating representations compare across forms, but distinct integers with magnitude above `2^53` can collapse to equality; strings compare by contents; arrays, objects, binaries, iterators, and exceptions compare by identity; `NaN` is not equal to itself.
- `==`, `!=`: in addition to strict matches, `null == undefined`; booleans compare as 0/1 against numbers or numeric strings; a number/string pair compares after a complete numeric-text parse. That parser ignores surrounding ASCII whitespace, treats empty text as 0, and accepts decimal/exponent syntax, `0x` hexadecimal (including a fraction and `p` exponent), and case-insensitive `NaN` or `Inf`/`Infinity`; other trailing characters make the comparison unequal. Containers and binaries are not converted to primitives or strings.
- `key in object` accepts only a string key. `index in array` accepts only a non-negative integer. A mismatched type or absent member returns `false`.
- `left && right` returns `left` when it is falsy; otherwise it evaluates and returns `right`. `left || right` returns `left` when it is truthy; otherwise it evaluates and returns `right`.
- `condition ? whenTrue : whenFalse` evaluates exactly one branch and returns it.

### 4.11 `if`, blocks, and expression statements

`if` and `else` bodies require braces. `else if` is supported:

```javascript
if ($.status >= 500) {
    return "server-error";
} else if ($.status >= 400) {
    return "client-error";
} else {
    return "ok";
}
```

Conditions use section 3.1. An expression may stand alone as a statement and its result is discarded; assignments and calls made for side effects commonly use this form.

### 4.12 `for-of`, `break`, and `continue`

```javascript
let result = [];

for (let key, value of $.items) {
    if (value == null) {
        continue;
    }
    array.push(result, value);
    if (length(result) >= 10) {
        break;
    }
}
```

Only the following two-variable form exists:

```typescript
// Array: key is a zero-based index, value is the element.
for (let key, value of array) { /* ... */ }

// Object: key is the property name and value is the property value,
// in insertion order.
for (let key, value of object) { /* ... */ }
```

- The collection expression is evaluated once. A value other than an array or object is treated as empty without throwing.
- Loop variables are scoped to the loop body and overwritten on each iteration.
- `continue` advances the innermost loop and `break` exits it. The current compiler treats either statement outside a loop as a no-op, but that is an implementation quirk and must not be used.
- Structurally adding/removing array elements or object properties during iteration throws a catchable `IterationError`. Updating an existing element/property value is not structural.

### 4.13 `return`

```javascript
return expression; // ScriptResultKind::Value, even when expression is undefined
return;            // ScriptResultKind::Void
```

Falling off the end also produces `Void`. A `return` inside a block, loop, or `try/catch` immediately ends the entire script.

### 4.14 `throw`, `try`, and `catch`

```javascript
try {
    let value = JSON.parse($.input);
    return value;
} catch (error) {
    return {ok: false, error};
}
```

`catch` requires a binding and braced body. An omitted binding and `finally` are not supported. `throw value;` may throw any script value. Standard-library type, range, and parse errors are also catchable; the catch binding receives the original exception value.

Lightweight built-in exceptions include `TypeError`, `RangeError`, `ReferenceError`, and `IterationError`. Some parse and HTTP operations allocate named/message exceptions such as `SyntaxError` or `Error`. There is no script API to inspect an exception's name, message, or position; a C++ host can inspect heap exceptions through the GC API.

C++ results distinguish two failure classes:

- `Exception` is a script-semantic error. Script `try/catch` can intercept it; an uncaught value becomes `ScriptResultKind::Exception`.
- `Abort` terminates the runtime and cannot be caught. Reasons include `OutOfMemory`, `InvalidState`, `InvalidOpcode`, `HostFault`, `Timeout`, `Cancelled`, and `Internal`.

If a mutating function aborts with `OutOfMemory` partway through an operation, changes already applied are not guaranteed to roll back.

### 4.15 Template strings

Backtick template strings are supported in scripts:

```javascript
let id = 42;
return `item-${id}-${1 + 2}`;
```

Templates may span lines and support most ordinary string escapes; `\$` inserts a literal `$`. `${expression}` may contain nested strings, objects, and templates. Each expression is converted with the primitive-text rules used by `+`, so interpolating an array, object, or binary throws `TypeError`. An empty template returns an empty string.

C++ also provides `compile_template_string()`. Its input is the template body without outer backticks:

```text
prefix-${$.name}-${length($.items)}
```

A compiled template always returns a string. `compile_template_string()` disables assignment by default, but does not automatically prohibit asynchronous functions. A host intending to use `exec_sync()` must check `Script::contains_async()`.

### 4.16 `directive`

`directive` is a compile-time host binding, not a runtime variable declaration:

```javascript
directive backend = http "@api";
// Generic syntax alias:
directive backend from http "@api";

return backend.request({path: "/health"});
```

- The forms are `directive name = type literal...;` and `directive name from type literal...;`. Arguments are `null`, boolean, number, or string literals with no commas between them.
- A directive name is unique within one compilation unit and must be declared before a call through it.
- The parser passes the declaration to `Library::resolve_directive_def()`. An unknown type, name, or literal combination is a compile error.
- A declaration emits no runtime instruction. Directive methods are still statically resolved by the host during compilation.
- The HTTP extension accepts exactly one string target; see section 6.4.

### 4.17 Explicitly unsupported syntax

The language does not support `const`/`var`, `while`/`do`, classic three-part `for`, one-variable `for-of`, `switch`, functions or arrow functions, classes, `new`, optional chaining, nullish coalescing, compound assignment, `++`/`--`, bitwise operations, regex literals, modules, Promise, or `await`.

The lexer recognizes some tokens for `++`, `--`, `~`, `#[`, and `^[`, but the parser/compiler gives them no executable semantics; using them fails compilation. `strings.match` and `strings.findAll` are also not registered.

## 5. Standard-library API

Constructing `fiber::script::std_lib::StdLibrary` registers the 45 call names below (46 overloads in total). In this section, an “exception” is a catchable script `Exception`; an “abort” is an uncatchable runtime `Abort`.

The following rules apply to every function:

- Names, overloads, and argument counts are resolved at compile time. An arity error never enters the function.
- A function does not mutate its arguments unless this section explicitly says it does. A newly returned string, binary, array, or object belongs to the current `GcHeap`.
- Any operation that allocates a result can abort with `OutOfMemory`; missing internal runtime state can abort with `InvalidState`. Those common aborts are not repeated for every entry.
- A variadic mutating function provides no transactional rollback if an allocation abort occurs partway through it.

### 5.1 General functions

#### `length(value?)`

```typescript
function length(value?: ScriptValue): number;
```

- Parameters: `value` defaults to `null`.
- Return: UTF-16 code-unit count for a string, byte count for `Binary`, element count for an array, or property count for an object. Every other type returns `0`. A malformed UTF-8 string borrowed from the host falls back to its byte length.
- Exceptions: none.
- Side effects: none.

#### `includes(container, ...items)`

```typescript
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

- Parameters: a string container requires every item to be a string; an array accepts any script value.
- Return: `true` when the string contains every substring, or when every item is found in the array using `===`. An invalid container/type combination returns `false`. A valid container with no items returns `true`.
- Exceptions: none; type mismatches return `false`.
- Side effects: none.

### 5.2 Array functions

#### `array.join(values, separator?)`

```typescript
function array.join(values: ScriptValue[], separator?: ScriptValue): string | undefined;
```

- Parameters: `values` must be an array. `separator` defaults to the empty string. Strings, numbers, and booleans use their textual form; `null`, `undefined`, containers, iterators, exceptions, and binary values use empty text.
- Return: a new joined string. Unlike JavaScript, the default separator is not a comma. The current implementation returns `undefined` if the final GC-string allocation fails.
- Exceptions: non-array `values` throws `TypeError`.
- Side effects: none.

#### `array.pop(values)`

```typescript
function array.pop(values: ScriptValue[]): ScriptValue | null;
```

- Parameters: `values` must be an array.
- Return: the removed last element, or `null` for an empty array.
- Exceptions: a non-array throws `TypeError`.
- Side effects: shrinks the array in place. This is structural mutation and causes a subsequent `IterationError` when performed while iterating that array.

#### `array.push(values, ...items)`

```typescript
function array.push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
```

- Parameters: `values` must be an array; `items` may be empty.
- Return: the same `values` array, not its new length.
- Exceptions: a non-array throws `TypeError`.
- Side effects: appends in place and structurally mutates the array. Items appended before an allocation abort are not rolled back.

### 5.3 String functions

String APIs intentionally use soft type failures: unless stated otherwise, a wrong text-argument type returns `false` or `null` instead of throwing `TypeError`. These functions require a strict UTF-8 view. A WTF-8 string containing an isolated UTF-16 surrogate is treated as a text-type failure, although language indexing, concatenation, and `length()` can still operate on it.

#### `strings.hasPrefix(text, prefix)` / `strings.hasSuffix(text, suffix)`

```typescript
function strings.hasPrefix(text: string, prefix: string): boolean;
function strings.hasSuffix(text: string, suffix: string): boolean;
```

- Parameters: both values must be strings; an empty prefix or suffix is valid.
- Return: a byte-equivalent Unicode text prefix/suffix test. Either non-string argument returns `false`.
- Exceptions: none.
- Side effects: none.

#### `strings.toLower(text)` / `strings.toUpper(text)`

```typescript
function strings.toLower(text: string): string | null;
function strings.toUpper(text: string): string | null;
```

- Parameters: `text` must be a string.
- Return: a new string with ASCII `A-Z`/`a-z` converted. Non-ASCII text is unchanged. A non-string returns `null`.
- Exceptions: none.
- Side effects: none.

#### `strings.trim(text, cutset?)`

```typescript
function strings.trim(text: string, cutset?: string | null): string | null;
```

- Parameters: `text` must be a string. An omitted, `null`, or non-string `cutset` selects default trimming. A string `cutset` is one complete substring, not a set of characters.
- Return: default mode removes bytes `<= 0x20` from both ends. Explicit mode repeatedly removes the complete `cutset` from both ends; an empty cutset removes nothing. A non-string `text` returns `null`.
- Exceptions: none.
- Side effects: none.

#### `strings.trimLeft(text, cutset?)` / `strings.trimRight(text, cutset?)`

```typescript
function strings.trimLeft(text: string, cutset?: string | null): string | null;
function strings.trimRight(text: string, cutset?: string | null): string | null;
```

- Parameters: the same as `strings.trim()`.
- Return: trims only the named side. Default whitespace is the ASCII ranges `0x09-0x0d` and `0x1c-0x20`; explicit mode repeatedly removes the complete substring. A non-string `text` returns `null`.
- Exceptions: none.
- Side effects: none.

#### `strings.split(text, separators?)`

```typescript
function strings.split(text: string, separators?: string | null): string[] | null;
```

- Parameters: `text` must be a string. An omitted, `null`, or non-string `separators` value means “do not split.” A string is a set of Unicode code points, any one of which is a separator.
- Return: without separators, a new one-element array containing the original string. With separators, adjacent/leading/trailing separators do not create empty fields; empty input returns an empty array. Non-string `text` returns `null`.
- Exceptions: none.
- Side effects: none.

#### `strings.contains(text, value)` / `strings.contains_any(text, chars)`

```typescript
function strings.contains(text: string, value: string): boolean | null;
function strings.contains_any(text: string, chars: string): boolean | null;
```

- Parameters: `contains` treats `value` as a complete substring; `contains_any` treats `chars` as a Unicode code-point set.
- Return: a matching boolean; either non-string argument returns `null`. Every string contains the empty substring, while an empty character set matches nothing.
- Exceptions: none.
- Side effects: none.

#### `strings.index(text, value)` / `strings.lastIndex(text, value)`

```typescript
function strings.index(text: string, value: string): number | null;
function strings.lastIndex(text: string, value: string): number | null;
```

- Parameters: both arguments must be strings; `value` is a complete substring.
- Return: the first/last matching UTF-16 starting index, `-1` if absent, or `null` on a type mismatch.
- Exceptions: none.
- Side effects: none.

#### `strings.indexAny(text, chars)` / `strings.lastIndexAny(text, chars)`

```typescript
function strings.indexAny(text: string, chars: string): number | null;
function strings.lastIndexAny(text: string, chars: string): number | null;
```

- Parameters: `chars` is a Unicode code-point set.
- Return: `indexAny` returns the first UTF-16 position of any candidate. `lastIndexAny` visits candidates in `chars` order and returns the last position of the first candidate that occurs; it is not necessarily the global maximum position across every candidate. Absence returns `-1`; a type mismatch returns `null`.
- Exceptions: none.
- Side effects: none.

#### `strings.repeat(text, count)`

```typescript
function strings.repeat(text: string, count: number): string | null;
```

- Parameters: `text` must be a string. Integer or floating `count` is accepted; floating point truncates toward zero and is used as a 32-bit count. `NaN` acts as zero. A negative count or non-number returns `null`.
- Return: the repeated string. Zero repetitions or empty input returns an empty string; one repetition may reuse the original string value.
- Exceptions/aborts: no catchable exception; a result over 16 MiB aborts with `OutOfMemory`.
- Side effects: none.

#### `strings.substring(text, start?, end?)`

```typescript
function strings.substring(text: string, start?: ScriptValue, end?: ScriptValue): string | null;
```

- Parameters: `text` must be a string. `start`/`end` default to 0/`2147483647` UTF-16 code units. Integers are direct, floating point truncates toward zero, booleans become 0/1, and `null`/`undefined`/unconvertible values become 0. A string parses a leading decimal integer and becomes 0 if parsing fails.
- Return: negative `start` clamps to 0; `start >= length` or `end <= start` returns an empty string; an excessive `end` clamps to the end. If a boundary falls between the surrogate units of a supplementary code point, the current implementation snaps it back to that code point's start rather than producing an isolated surrogate as string indexing can. Non-string `text` returns `null`.
- Exceptions: none.
- Side effects: none.

#### `strings.toString()` / `strings.toString(value)`

```typescript
function strings.toString(): string;
function strings.toString(value: ScriptValue): string;
```

- Parameters: zero and one argument are separate overloads.
- Return: no argument produces `""`; `null` and `undefined` produce `"null"`; scalar values use compatibility text; arrays/objects produce `"<ArrayNode>"`/`"<ObjectNode>"`; `Binary` contributes its raw bytes. This is not JSON serialization.
- Exceptions: none; creating the result may use the common abort behavior.
- Side effects: none.

`strings.match` and `strings.findAll` are not currently registered.

### 5.4 Binary functions

#### `binary.base64Encode(value)`

```typescript
function binary.base64Encode(value: Binary): string | undefined;
```

- Parameters/return: standard Base64 for a `Binary`; any other type returns `undefined`. Empty binary returns an empty string.
- Exceptions: none.
- Side effects: none.

#### `binary.base64Decode(value)`

```typescript
function binary.base64Decode(value: string): Binary | undefined;
```

- Parameters/return: strictly decodes standard Base64 into a new `Binary`; a non-string returns `undefined`.
- Exceptions: an illegal character, whitespace, invalid padding, or non-multiple-of-four length throws `RangeError`.
- Side effects: none.

#### `binary.hex(value)`

```typescript
function binary.hex(value: Binary): string;
```

- Parameters/return: returns lowercase hexadecimal for a binary value; empty binary returns an empty string.
- Exceptions: a non-binary argument throws `TypeError`.
- Side effects: none.

#### `binary.fromHex(value)`

```typescript
function binary.fromHex(value: string): Binary;
```

- Parameters/return: strictly decodes an even-length, case-insensitive hexadecimal string into a new binary value.
- Exceptions: a non-string throws `TypeError`; odd length or an invalid character throws `RangeError`.
- Side effects: none.

#### `binary.getUtf8Bytes(value)`

```typescript
function binary.getUtf8Bytes(value: ScriptValue): Binary;
```

- Parameters/return: copies compatibility text into a new binary. Strings contribute UTF-8, scalars their text, `null` the bytes for `null`, arrays/objects the node placeholders, and `Binary` its raw bytes. `undefined`, iterators, and exceptions contribute empty bytes.
- Exceptions: none.
- Side effects: none.

### 5.5 Hash functions

#### `hash.crc32(value)`

```typescript
function hash.crc32(value: ScriptValue): number;
```

- Parameters: uses scalar compatibility text. `null` is `"null"`; arrays, objects, binaries, `undefined`, iterators, and exceptions are empty text.
- Return: CRC-32 from 0 through `0xffffffff`; empty text produces 0.
- Exceptions: none.
- Side effects: none.

#### `hash.md5(value)` / `hash.sha1(value)` / `hash.sha256(value)`

```typescript
function hash.md5(value: string | Binary): string;
function hash.sha1(value: string | Binary): string;
function hash.sha256(value: string | Binary): string;
```

- Parameters: strings use UTF-8 bytes and binary values use raw bytes.
- Return: respectively 32, 40, or 64 lowercase hexadecimal characters.
- Exceptions/aborts: another type throws `TypeError`; a digest backend failure aborts with `HostFault`.
- Side effects: none.

MD5 and SHA-1 are provided for interoperability, not collision-resistant security designs.

### 5.6 Math and random functions

#### `math.floor(value)`

```typescript
function math.floor(value: number): number;
```

- Parameters/return: an integer is returned unchanged; a finite floating-point value representable by `int64_t` is rounded toward negative infinity and returned as an integer. The current implementation gives no portable contract for `NaN`, infinity, or a floating value outside the `int64_t` range; callers should exclude them.
- Exceptions: a non-number throws `TypeError`.
- Side effects: none.

#### `math.abs(value)`

```typescript
function math.abs(value: number): number;
```

- Parameters/return: absolute value of an integer or floating-point number. Floating `NaN` remains `NaN`, and either infinity becomes positive infinity. For Java compatibility, `INT64_MIN` remains negative and unchanged.
- Exceptions: a non-number throws `TypeError`.
- Side effects: none.

#### `rand.random(max?)`

```typescript
function rand.random(max?: number): number;
```

- Parameters: `max` defaults to 1000 and must be numeric. Floating point truncates toward zero and saturates to the 64-bit range.
- Return: a uniform integer in `[0, max)` from the current thread's PRNG. It is not cryptographically secure and offers no seed API.
- Exceptions: a non-number throws `TypeError`; a truncated `max <= 0` throws `RangeError`.
- Side effects: advances per-thread pseudorandom state.

#### `rand.canary(ratio, ...keys)`

```typescript
function rand.canary(ratio: ScriptValue, ...keys: ScriptValue[]): boolean;
```

- Parameters: numeric `ratio` truncates toward zero, booleans become 0/1, and other types become 0. A ratio `<= 0` is always false and `>= 100` is always true.
- Return: without keys, selects a random bucket in `[0, 100)`. With keys, accumulates non-empty compatibility text into CRC-32 in argument order and selects `crc % 100`. A null key is `"null"`; containers, binaries, and `undefined` are empty and skipped. Key order matters.
- Exceptions: none.
- Side effects: only a no-key call with ratio 1..99 advances random state. A keyed call is deterministic.

### 5.7 JSON functions

#### `JSON.parse(text)`

```typescript
function JSON.parse(text: string): ScriptValue;
```

- Parameters/return: parses one complete JSON document. A JSON number becomes integer or floating point according to notation and range. Object order follows the input; a duplicate key keeps its last value at the first occurrence's position.
- Exceptions: a non-string throws `TypeError`. Invalid JSON throws an allocated exception with `name = "SyntaxError"`, a message, and byte offset.
- Side effects: does not mutate input; allocates the complete result tree in the current heap.

#### `JSON.stringify(value)`

```typescript
function JSON.stringify(value: ScriptValue): string | undefined;
```

- Parameters/return: top-level `undefined` returns `undefined`. Top-level `NaN` and either infinity return the string `"null"`. Other values produce compact JSON. Nested `undefined` becomes JSON null, `Binary` becomes a Base64 string, an exception becomes an object with `position`, `name`, `message`, and `meta`, and ordinary object properties retain insertion order.
- Exceptions: an invalid string, nested non-finite number, iterator, or other unencodable value throws `TypeError`.
- Side effects: none.

### 5.8 Object functions

#### `Object.assign(target, source, ...sources)`

```typescript
function Object.assign(
    target: ScriptObject,
    source: ScriptValue,
    ...sources: ScriptValue[]
): ScriptObject;
```

- Parameters: `target` must be an object. A non-object source is silently skipped. Sources and their properties are processed in argument/insertion order.
- Return: the same `target`.
- Exceptions: a non-object target throws `TypeError`.
- Side effects: adds or overwrites properties in place; overwriting keeps the original order position. Fields copied before OOM are not rolled back.

#### `Object.keys(value)` / `Object.values(value)`

```typescript
function Object.keys(value: ScriptObject): string[];
function Object.values(value: ScriptObject): ScriptValue[];
```

- Parameters/return: creates a new key/value array in property insertion order; values are shallow copies.
- Exceptions: a non-object throws `TypeError`.
- Side effects: does not modify the object.

#### `Object.deleteProperties(target, key, ...keys)`

```typescript
function Object.deleteProperties(
    target: ScriptObject,
    key: ScriptValue,
    ...keys: ScriptValue[]
): ScriptObject;
```

- Parameters: `target` must be an object. Only string keys take effect; non-strings and absent keys are skipped.
- Return: the same `target`.
- Exceptions: a non-object target throws `TypeError`.
- Side effects: removes properties in place, which is structural mutation. The current low-level delete treats failure as a no-op.

### 5.9 URL form functions

These APIs implement `application/x-www-form-urlencoded`, not ECMAScript `encodeURIComponent`: a space encodes as `+`, and `+` decodes as a space.

#### `URL.encodeComponent(value)`

```typescript
function URL.encodeComponent(value: string): string;
```

- Parameters/return: letters, digits, `-`, `_`, `.`, and `*` are preserved; a space becomes `+`; every other UTF-8 byte becomes uppercase `%HH`.
- Exceptions: a non-string throws `TypeError`.
- Side effects: none.

#### `URL.decodeComponent(value)`

```typescript
function URL.decodeComponent(value: string): string;
```

- Parameters/return: replaces `+` with a space and decodes `%HH` escapes. Invalid UTF-8 in the decoded bytes is replaced with U+FFFD.
- Exceptions: a non-string throws `TypeError`; an incomplete or invalid percent escape throws `RangeError`.
- Side effects: none.

#### `URL.parseQuery(value)`

```typescript
function URL.parseQuery(value: string): {
    [key: string]: string | string[];
};
```

- Parameters/return: parses a query string without `?`. Empty `&` segments are skipped; a field without `=` has an empty value; later `=` characters remain in the value. A repeated key starts as a string, promotes to an array on its second occurrence, and appends thereafter. Invalid decoded UTF-8 is replaced with U+FFFD.
- Exceptions: a non-string throws `TypeError`; an invalid percent escape in any key/value throws `RangeError`.
- Side effects: returns a new object and any required arrays; does not modify input.

#### `URL.buildQuery(value?)`

```typescript
function URL.buildQuery(
    value?: null | undefined | { [key: string]: ScriptValue | ScriptValue[] }
): string | null | undefined;
```

- Parameters: defaults to `undefined`. Object properties use insertion order; an array value expands into repeated keys and an empty array emits nothing. Values use compatibility text: `null` is `null`, containers use node placeholders, and binary contributes raw bytes.
- Return: a form-urlencoded query string. A `null` or `undefined` argument is returned unchanged.
- Exceptions: any other non-object throws `TypeError`.
- Side effects: none.

## 6. HTTP script API

HTTP bindings are not part of the default `StdLibrary`. The C++ host must call `register_http_functions_to_lib()` and pass the current request-and-heap-bound `ScriptExchangeCtx*` as the execution `attach` pointer.

The complete registered names are:

```text
req.getHeader      req.getQuery       req.getCookie       req.getUri
req.getPath        req.getQueryStr    req.getMethod       req.readJson
req.readBinary     req.discardBody
resp.setHeader     resp.addHeader     resp.addCookie      resp.sendJson
resp.send
```

Common constraints are:

- A missing/invalid `ScriptExchangeCtx` causes an uncatchable `InvalidState` abort. Upstream functions also require `HttpScriptServices` to be configured on the context.
- `req.readJson()`, `req.readBinary()`, `req.discardBody()`, `resp.sendJson()`, `resp.send()`, and directive `request()`/`proxyPass()` are asynchronous. Their scripts require `exec_async()`.
- HTTP I/O, encoding, and configuration failures generally throw a catchable exception named `Error`. Allocating a value or exception can still abort with `OutOfMemory`.
- Network, request-body, and response state is not transactional. An exception does not undo bytes already consumed, an upstream request already issued, or downstream headers/body already written.

### 6.1 Request functions: `req.*`

```typescript
declare namespace req {
    function getHeader(): { [name: string]: string } | undefined;
    function getHeader(name: string): string | undefined | null;

    function getQuery(): { [name: string]: string } | undefined;
    function getQuery(name: string): string | undefined;

    function getCookie(): { [name: string]: string } | undefined;
    function getCookie(name: string): string | undefined;

    function getUri(): string;
    function getPath(): string;
    function getQueryStr(): string;
    function getMethod(): string;

    // Async host functions, still written as ordinary calls in scripts.
    function readJson(): ScriptValue;
    function readBinary(): Binary;
    function discardBody(): null;
}
```

#### `req.getHeader()` / `req.getHeader(name)`

- Parameters: the no-argument overload reads all request headers. The one-argument overload requires a non-empty string and performs a case-insensitive HTTP header lookup.
- Return: the no-argument overload lazily builds and caches an object. Keys retain case-sensitive header spelling; an exactly repeated key keeps the last value, while differently cased names can coexist. Cache allocation failure can return `undefined`. The named overload returns the field value, `undefined` when absent, and `null` for an empty or non-string name.
- Exceptions/aborts: no catchable exception; missing context is `InvalidState`, and result allocation failure is `OutOfMemory`.
- Side effects: the no-argument form creates a persistent GC root/cache in the context. Neither form consumes the body or mutates the request.

#### `req.getQuery()` / `req.getQuery(name)`

- Parameters: the no-argument overload parses the complete raw query; the named overload expects a non-empty string and performs an exact, case-sensitive lookup of the decoded key.
- Return: the no-argument overload caches a form-decoded object and keeps the last repeated key; allocation failure can return `undefined`. The named overload returns the cached value, or `undefined` for an absent, empty, or non-string key.
- Exceptions/aborts: malformed `%HH` currently does not throw; parsing stops and preserves fields decoded before the error. Missing context and lookup-root allocation failures abort with `InvalidState`/`OutOfMemory`.
- Side effects: the first call parses and caches the object. `+` decodes as a space, and invalid UTF-8 becomes U+FFFD. The body is untouched.

#### `req.getCookie()` / `req.getCookie(name)`

- Parameters: the no-argument overload parses every `Cookie` request header. It splits on `;`, trims surrounding spaces/tabs, and splits name/value at the first `=`; a segment without `=` has an empty value. It does not percent-decode or unquote. The named overload expects a non-empty cookie name and uses an exact, case-sensitive lookup.
- Return: the no-argument overload caches an object in which a later identical cookie name overwrites an earlier value; allocation failure can return `undefined`. The named overload returns `undefined` for an absent, empty, or non-string name.
- Exceptions/aborts: cookie syntax does not throw; missing context or lookup-root failure aborts with `InvalidState`/`OutOfMemory`.
- Side effects: first use parses and caches the cookie object. The body is untouched.

#### `req.getUri()` / `req.getPath()` / `req.getQueryStr()` / `req.getMethod()`

| Function | Return value |
| --- | --- |
| `req.getUri()` | Raw request target `path[?query]`, without scheme or host |
| `req.getPath()` | Parsed path |
| `req.getQueryStr()` | Raw query without `?`, or an empty string |
| `req.getMethod()` | Current HTTP method name |

These four functions take no arguments, throw no catchable exception, and do not mutate the request. Missing context and string allocation failure abort with `InvalidState`/`OutOfMemory`.

#### `req.readJson()`

- Parameters: none; asynchronous.
- Return: reads the entire request body and constructs the corresponding script value.
- Exceptions: an empty body, body-read failure, or invalid JSON throws `Error` with a fixed message. The detailed JSON byte offset is not exposed.
- Side effects: consumes the request body and materializes the complete body and decoded tree in memory. The function sets no size limit.

#### `req.readBinary()`

- Parameters: none; asynchronous.
- Return: a new `Binary` containing the complete request body; an empty body produces a zero-length binary.
- Exceptions: a read failure throws `Error`; allocation failure aborts with `OutOfMemory`.
- Side effects: consumes the body and materializes it contiguously in memory, without its own size limit.

#### `req.discardBody()`

- Parameters: none; asynchronous.
- Return: always `null`.
- Exceptions: the current implementation ignores an underlying drain error and throws no catchable exception. Missing context aborts with `InvalidState`.
- Side effects: consumes and discards the remaining request body.

The request body is a one-shot stream. Do not combine these three operations or read the body before a `service.proxyPass()` that needs to forward it. Public services should enforce request-body limits in the HTTP layer.

### 6.2 Response functions: `resp.*`

```typescript
type HeaderTextValue = string | number | boolean | null;

interface ResponseCookie {
    name: string;
    value?: ScriptValue;
    domain?: string;
    path?: string;
    maxAge?: number;
    secure?: boolean;
    httpOnly?: boolean;
    sameSite?: "Lax" | "Strict" | "None";
}

declare namespace resp {
    function setHeader(name: string, value: HeaderTextValue): null;
    function addHeader(name: string, value: HeaderTextValue): null;
    function addCookie(cookie: ResponseCookie): boolean;

    // Async host functions.
    function sendJson(status: number, body: ScriptValue): null;
    function send(status: number): null;
    function send(status: number, body: ScriptValue): null;
}
```

All three send overloads honor only an integer status and otherwise use 200. The script binding does not prevalidate the HTTP status-code range; a protocol-layer rejection is reported as a send `Error`.

#### `resp.setHeader(name, value)`

- Parameters: `name` must be a non-empty string. `value` uses scalar text conversion: strings, integers, floats, booleans, and `null` produce their textual representation.
- Return: `null` on success.
- Exceptions: a non-string/empty name or a value converting to empty text throws `Error`. Therefore an empty string, `undefined`, array, object, `Binary`, iterator, or exception cannot be a header value. A protocol-layer rejection of the final name/value can also surface as `Error` while sending.
- Side effects: before headers are sent, replaces every pending header with the same name. After they are sent, silently does nothing and still returns `null`.

#### `resp.addHeader(name, value)`

- Parameters, return, and exceptions: the same as `resp.setHeader()`.
- Side effects: before headers are sent, appends a field and allows repeated names. Afterward, silently does nothing.

#### `resp.addCookie(cookie)`

- Parameters: the argument must be an object. `name` must be a non-empty valid HTTP-token string. `value` uses scalar text conversion; missing/unconvertible values become empty. `domain`/`path` apply only when strings. Only a non-negative integer `maxAge` is emitted. `secure`/`httpOnly` require boolean `true`. `sameSite` requires an exact case-sensitive `"Lax"`, `"Strict"`, or `"None"`.
- Return: `true` when encoding succeeds; `false` for a non-object or missing/wrong/invalid name. A wrong type for another field normally just omits that attribute.
- Exceptions: none catchable; missing context aborts with `InvalidState`.
- Side effects: appends a pending `Set-Cookie` on success. If headers were already sent, the append is ignored but the function may still return `true`; the return value reports encoding success only.

#### `resp.sendJson(status, body)`

- Parameters: asynchronous. Only an integer `status` is honored; another type falls back to 200. `body` may be any JSON-encodable script value.
- Return: `null` after a successful send. `undefined` encodes as JSON null, `Binary` as a Base64 string, and an exception value as an object with `position`, `name`, `message`, and `meta`.
- Exceptions: an invalid string, non-finite float, iterator, or other encode failure, and header/body I/O failures, throw `Error`.
- Side effects: replaces Content-Type with `application/json`, commits all pending headers, writes a fixed-Content-Length body, and ends the response stream.

#### `resp.send(status)`

- Parameters: asynchronous; a non-integer status falls back to 200.
- Return: `null` after success.
- Exceptions: header or end-of-stream failure throws `Error`.
- Side effects: commits pending headers, sends a zero-length body, and ends the response. No Content-Type is added.

#### `resp.send(status, body)`

- Parameters: asynchronous; a non-integer status falls back to 200.
- Return: `null` after success.
- Encoding: `Binary` is sent as raw bytes with no automatic Content-Type; a string sets `text/plain;charset=utf-8` and uses UTF-8; every other value follows `resp.sendJson()` JSON encoding and sets `application/json`.
- Exceptions: encoding, header, or body I/O failure throws `Error`.
- Side effects: commits pending headers, writes the body, and ends the response.

A normal script should call exactly one send function. Once the header is committed, the context marks the response sent; later header mutations are no-ops, while another `send*()` generally becomes an I/O error. If body writing fails after the header commit, the error remains catchable, but bytes already delivered to the client cannot be rolled back.

### 6.3 Route and connection constants

`RouteScriptExtension` and `ExchangeConstExtension` provide compile-time-resolved, request-time values:

```typescript
declare const $path: { [routeVariable: string]: string | null };
declare const $query: { [queryName: string]: string | null };
declare const $header: { [normalizedHeaderName: string]: string | null };
declare const $cookie: { [normalizedCookieName: string]: string | null };
declare const $context: { [hostContextName: string]: string | null };

declare const $req: {
    uri: string;     // path[?query]
    method: string;
    path: string;
    query: string;   // without '?'
};

declare const $conn: {
    remote_addr: string | null;
    remote_port: number;
    http_version: "HTTP/0.9" | "HTTP/1.0" | "HTTP/1.1" | "HTTP/2" | "HTTP/3";
    scheme: string;
    tls: boolean;
};
```

Example:

```javascript
resp.sendJson(200, {
    id: $path.id,
    source: $query.source,
    forwardedFor: $header.x_forwarded_for,
    session: $cookie.session,
    method: $req.method,
    remoteAddress: $conn.remote_addr,
    tls: $conn.tls
});
```

Resolution rules:

- `$path.<name>` must exactly match a route variable supplied to `RouteScriptExtension::CompileScope`, or compilation fails. The other four dynamic namespaces accept any valid identifier key. `$req`/`$conn` accept only the fixed fields above; unknown fields also fail compilation.
- Package names in `$path`, `$query`, `$header`, `$cookie`, and `$context` are ASCII-lowercased and convert `-` to `_` before matching/deduplication. Thus `$header.x_forwarded_for` matches `X-Forwarded-For`, and `$query.foo_bar` can match query key `Foo-Bar`. Names with the same normalized form share a slot.
- The key after `.` must be a script identifier; `$query["a.b"]` is not supported. Use `req.getQuery("...")`, `req.getHeader("...")`, or `req.getCookie("...")` when exact case, punctuation, or `-` versus `_` matters.
- Dynamic slots start as `null`: a missing path/query/header/cookie/context value returns `null`. Repeated query values keep the last match; repeated headers/cookies keep the first match. The host binds path/context values.
- `$req.uri` matches `req.getUri()`, and `$req.query` excludes `?`. `$conn.remote_addr` is `null` if address conversion fails. Other `$req`/`$conn` fields read directly from the exchange and do not enter the `ConstPackage`.
- Reads have no script side effects or catchable exceptions. Missing `prepare_constants()`, package identity mismatch, or missing context causes `InvalidState`; memory failure while preparing dynamic values is reported by the host initialization path, while request-pool failure during first formatting of `$conn.remote_addr` aborts with `OutOfMemory`.

`ConstPackage::Builder` deduplicates dynamic names and assigns compact indexes. At request time, prepare slots from the final immutable package before binding route/context values; see section 8.2.

### 6.4 Upstream HTTP directives

An upstream target must first be bound by a directive. There is no independent global `http.request()` or `http.proxyPass()`:

```javascript
directive backend = http "@api";

let response = backend.request({
    method: "GET",
    path: "/v1/items",
    query: {page: 1, tag: ["a", "b"]},
    includeHeaders: true,
    timeout: 5000
});

resp.sendJson(200, {
    upstreamStatus: response.status,
    upstreamHeaders: response.headers,
    upstreamBodyBase64: binary.base64Encode(response.body)
});
```

Targets can be named upstreams or URL authorities without paths:

```javascript
directive a = http "@backend";
directive b = http "backend";
directive c = http "http://127.0.0.1:8080";
directive d = http "https://api.example.com";
```

Target rules:

- A value beginning with `http://`/`https://` is a fixed URL authority and cannot contain a path, query, or fragment. Write only `host[:port]`. Userinfo has no dedicated semantics and must not be used. Omitted ports default to 80/443.
- A bracketed IPv6 authority currently requires an explicit port, for example `http://[::1]:8080`. A port is decimal in 0..65535; zero selects the scheme default.
- Every other non-empty string is a named upstream, with optional leading `@`. `HttpScriptServices` resolves whether it exists at request time.
- Each directive method accepts zero or one options argument and does not accept argument spread. A non-object options value behaves like an empty object.
- `options.url` is always a request `path?query`, never a host. Supplying a complete HTTP(S) URL there throws a catchable `Error`.

#### `service.request()`

```typescript
type RequestHeaderOverrides = {
    [name: string]: ScriptValue | null | undefined;
};

interface HttpRequestOptions {
    url?: string; // Complete path[?query], takes precedence over path/query.
    path?: string;
    query?: string | { [name: string]: ScriptValue | ScriptValue[] };
    method?: "GET" | "POST" | "PUT" | "DELETE" | "HEAD" | "OPTIONS" | "PATCH" | string;
    headers?: RequestHeaderOverrides;
    body?: ScriptValue;
    timeout?: number; // Positive integer milliseconds; default 30000.
    includeHeaders?: boolean;
}

interface HttpRequestResult {
    status: number;
    headers?: { [name: string]: string };
    body: Binary;
}

interface HttpService {
    request(options?: HttpRequestOptions): HttpRequestResult;
}
```

Option fields:

| Field | Rule |
| --- | --- |
| `method` | Case-insensitive GET, POST, PUT, DELETE, HEAD, OPTIONS, or PATCH; absent/non-string/unknown values fall back to GET |
| `url` | A non-empty complete `path?query`, taking precedence over `path`/`query`; a full HTTP(S) URL is rejected |
| `path` | A non-empty string when `url` is absent; otherwise defaults to `/` |
| `query` | A non-empty string is appended verbatim; an object is form-encoded and array properties become repeated keys; another type or empty result omits the query |
| `headers` | Object entries set headers; `null`/`undefined` removes one, and a value converting to empty compatibility text is silently skipped |
| `body` | Encoded below; absent, `null`, empty string, or empty `Binary` means no body |
| `timeout` | Only a positive integer applies; floating point, non-positive, and other values use 30000 ms |
| `includeHeaders` | Only a boolean applies; default `false` |

Object query/header values use `node_json_to_string` compatibility text: scalars use their text, `null` is `"null"`, arrays/objects use node placeholders, and binary contributes raw bytes. Each element of an array-valued query field is encoded separately; an empty array emits no pair. Final HTTP syntax validation belongs to the protocol layer.

Body encoding and automatic Content-Type:

| Body type | Encoding | Content-Type when not explicit |
| --- | --- | --- |
| `Binary` | Raw bytes | `application/octet-stream` |
| String | UTF-8 bytes | `text/plain;charset=utf-8` |
| Object and explicit Content-Type contains lowercase `application/x-www-form-urlencoded` | Form encoding | Existing value remains |
| Other object, array, number, or boolean | JSON | `application/json;charset=utf-8` |

The Content-Type substring check is currently case-sensitive. An empty encoded JSON/form body is treated as no body. A JSON encoding failure also currently degrades to no body instead of throwing, so do not supply an iterator, invalid string, or non-finite number as the request body.

- Return: a new `{status, body}` object after success. `status` is the final non-1xx upstream status and `body` is the complete response as `Binary`. `headers` is present only when `includeHeaders === true`; an exactly repeated response-header key keeps the last value.
- Exceptions/aborts: a complete URL in `options.url`, pool acquisition, DNS/connect, header/body write, response read, or timeout failure throws `Error`. Missing services aborts with `InvalidState`; result allocation failure aborts with `OutOfMemory`.
- Side effects: acquires/creates an upstream connection, sends one upstream request, and completely reads its response. Request and response bodies are contiguous buffers with no function-level limit; prefer `proxyPass()` for large or pure-forwarding responses.

#### `service.proxyPass()`

```typescript
interface ProxyPassOptions {
    url?: string;
    path?: string;
    query?: string | { [name: string]: ScriptValue | ScriptValue[] };
    method?: string;
    headers?: RequestHeaderOverrides;
    responseHeaders?: RequestHeaderOverrides;
    timeout?: number; // Integer milliseconds; default 30000.
    flush?: boolean;
    websocket?: boolean;
}

interface HttpService {
    proxyPass(options?: ProxyPassOptions): number;
}
```

`proxyPass()` streams the inbound request to the bound upstream and streams its response downstream. Option rules are:

| Field | Rule |
| --- | --- |
| `method` | Absent/non-string inherits the inbound method; one of the seven recognized methods overrides it; an unknown string also falls back to inbound |
| `url` | A non-empty complete `path?query`, overriding `path`/`query`; a full HTTP(S) URL is rejected |
| `path` | A non-empty string when `url` is absent; otherwise inherits inbound path |
| `query` | A non-empty string replaces inbound query; an object form-encodes a replacement (an empty object removes it); another type or empty string inherits inbound query |
| `headers` | Sets/removes fields after copying inbound request headers; conversion matches `request()`. Request framing fields are removed again afterward |
| `responseHeaders` | Sets/removes fields after filtering upstream response headers; conversion matches `headers` |
| `timeout` | Only a positive integer applies; default 30000 ms. Used for upstream acquisition, connection, I/O, and ordinary response reads |
| `flush` | Only boolean `true` enables the low-latency body pipe; default `false` |
| `websocket` | Only boolean `true` enables WebSocket proxying; default `false` |

Ordinary HTTP behavior:

- Inbound request headers are copied after filtering framing and hop-by-hop fields, then `headers` overrides apply. The request body streams directly from inbound to upstream, so there is no `body` option and the inbound body is consumed.
- Upstream 1xx responses are skipped until a final response. Hop-by-hop response fields are filtered before `responseHeaders`. HEAD and statuses that disallow a body send no downstream body and attempt to discard any remaining upstream body.
- A known Content-Length preserves fixed-length framing; otherwise automatic framing is used. The response body pipe defaults to a 64 KiB buffer and 48 KiB low-water mark, waiting for more data or EOF below that threshold.
- `flush: true` sets low-water to zero and disables aggregation across reads. It reads at most 64 KiB, then fully writes and flushes each chunk before reading the next; when gzip is active, the flush uses `Z_SYNC_FLUSH`. It does not add `X-Accel-Buffering: no` or disable buffering in another proxy/protocol layer. Ordinary downstream body writes do not use the `timeout` deadline.

WebSocket behavior:

- `websocket: true` requires a valid inbound HTTP/1.1 WebSocket Upgrade or HTTP/2/3 Extended CONNECT request; otherwise it throws before connecting upstream.
- Upstream method is forced to GET. Any non-empty method not recognized as GET, including an unknown method, throws `Error`. The function establishes upstream HTTP/1.1 Upgrade, sends 101 to an HTTP/1.1 downstream, and sends 200 for Extended CONNECT.
- Required handshake fields are restored after custom header overrides and cannot be removed through options. After a successful handshake the function waits for the bidirectional tunnel; `timeout` is the per-operation tunnel read/write timeout.
- The return remains upstream 101 even when an Extended CONNECT client saw 200. Current tunnel-relay termination/errors are not converted to script exceptions; after a successful handshake the function returns 101.

Return, failure, and side effects:

- Return: ordinary mode returns the final upstream status. WebSocket mode returns upstream 101. Before return, the downstream response has been sent or the tunnel has ended.
- Exceptions/aborts: URL, WebSocket handshake, connection, upstream/downstream header, request-body, response-body, I/O, and timeout failures throw `Error`; missing services aborts with `InvalidState`.
- Side effects: consumes the inbound request body, sends an upstream request, and commits/writes the downstream response. The context is marked sent immediately after the response header. A later streaming error remains catchable, but a partially delivered response cannot be replaced.

Normally make `service.proxyPass()` the final meaningful action, for example `return service.proxyPass({flush: true});`. Do not call `resp.send*()` after success.

## 7. Embedding scripts in C++

### 7.1 CMake linkage

In-tree applications link `fiber_lib`; its public `include/` path is propagated to consumers:

```cmake
add_executable(script_embed main.cpp)
target_link_libraries(script_embed PRIVATE fiber_lib)
```

Include core APIs through their `<fiber/...>` paths.

### 7.2 Compile and execute synchronously

```cpp
#include <cstdio>
#include <utility>

#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/std/StdLibrary.h>

int main() {
    using namespace fiber::script;

    // Use a separate instance when adding host APIs. For an unmodified
    // standard library, std_lib::StdLibrary::instance() is also available.
    std_lib::StdLibrary library;
    GcHeap heap;

    // LocalMark releases temporary root slots together at scope exit.
    GcHeap::LocalMark mark(heap);
    ValueHandle root = heap.local_value();
    if (!root) {
        return 1;
    }

    *root = JsValue::make_object(heap, 2);
    if (js_value_type(*root) != JsNodeType::Object) {
        return 1;
    }

    JsValue name = JsValue::make_string(heap, "fiber", 5);
    if (js_value_type(name) != JsNodeType::String ||
        !gc_object_set_key(&heap, root, "name", 4, name) ||
        !gc_object_set_key(&heap, root, "count", 5,
                           JsValue::make_integer(3))) {
        return 1;
    }

    auto compiled = compile_script(
            library,
            R"(
                return {
                    greeting: "hello " + $.name,
                    next: $.count + 1
                };
            )");
    if (!compiled) {
        std::fprintf(stderr, "compile error at %zu: %s\n",
                     compiled.error().position,
                     compiled.error().message.c_str());
        return 1;
    }

    Script script = std::move(*compiled);
    if (script.contains_async()) {
        std::fprintf(stderr, "unexpected async script\n");
        return 1;
    }

    ScriptResult result = script.exec_sync(*root, nullptr, heap);
    if (!result.is_value()) {
        std::fprintf(stderr, "script did not return a value\n");
        return 1;
    }

    // The VM no longer roots the returned heap value after execution. Root
    // it before any later operation that might allocate in this heap.
    ValueHandle returned = heap.local_value();
    if (!returned) {
        return 1;
    }
    *returned = result.value();

    // Inspect *returned with js_value_type(), gc_object_get_key(), and so on.
    return 0;
}
```

The compilation entry point is:

```cpp
std::expected<Script, parse::ParseError>
compile_script(Library &library,
               std::string_view source,
               bool allow_assign = true,
               std::size_t max_depth = 128);
```

- `library` defines the functions, constants, and directives visible at compile time.
- `allow_assign=false` rejects assignment expressions, which is useful for conditions and read-only configuration expressions.
- `max_depth` limits both parser and compiler nesting; 0 is treated as 1.
- `ParseError::position` is a source position and `message` is a short diagnostic.

### 7.3 Execution results

```typescript
type ScriptExecutionResult =
    | { kind: "Value"; value: ScriptValue }
    | { kind: "Void" }
    | { kind: "Exception"; exception: ScriptValue }
    | { kind: "Abort"; reason: ScriptAbortReason; position: number };
```

The C++ `ScriptResultKind` cases are:

- `Value`: the script executed `return expression;`. This remains `Value` when the expression is `undefined`.
- `Void`: the script executed `return;` or reached the end. Both `Value` and `Void` satisfy `is_success()`.
- `Exception`: an uncaught script exception, available through `exception()`.
- `Abort`: a runtime abort, available through `abort().reason` and `abort().position`.

Do not use `has_value()` alone as the success test: `Void` is successful but carries no value. Branch on `kind` first.

### 7.4 Asynchronous execution

```cpp
fiber::async::Task<fiber::script::ScriptResult>
run(fiber::script::Script &script,
    fiber::script::JsValue root,
    void *attach,
    fiber::script::GcHeap &heap) {
    co_return co_await script.exec_async(root, attach, heap);
}
```

- `exec_async()` can execute either a synchronous or asynchronous script.
- `exec_sync()` encountering an asynchronous opcode triggers `FIBER_PANIC`; check `contains_async()` first.
- `root`, `attach`, `heap`, and data referenced by `attach` must live until the returned coroutine completes.
- Do not concurrently execute scripts in the same `GcHeap`. Give each request its own heap or use a clearly serialized owner.

### 7.5 GC roots and lifetime

Every heap object belongs to the `GcHeap` that created it:

- Use `ValueHandle`, not a bare `JsValue`, for a temporary heap value that must survive later allocations.
- `heap.local_value()` allocates a temporary root slot; `GcHeap::LocalMark` releases a group of them. `heap.global_value()` allocates a root lasting until heap destruction.
- During VM execution, the root argument, variables, stack, constant caches, and asynchronous arguments are rooted automatically.
- After the VM returns, a value inside `ScriptResult` is no longer automatically rooted. Copy it immediately to a local/global root if consuming it can allocate in the same heap.
- `JsValue::make_native_string()` and `make_native_binary()` borrow storage. The host must keep that storage alive for every use.
- `JsValue::make_string()` and `make_binary()` copy their contents into the GC heap.
- Never retain or dereference a heap-backed `JsValue` after destroying its heap.
- Some compile-time string results borrow compiled `Script` data. Keep both the `Script` and `GcHeap` alive until result consumption finishes.

### 7.6 Register a synchronous host function

```cpp
#include <fiber/script/Library.h>
#include <fiber/script/ScriptResult.h>
#include <fiber/script/std/StdLibrary.h>

namespace {

using fiber::script::AbiResult;
using fiber::script::JsNodeType;
using fiber::script::JsValue;
using fiber::script::Library;

AbiResult add(void *userdata,
              const Library::HostCallFrame &frame,
              Library::Arguments args) noexcept {
    (void) userdata;
    (void) frame;

    if (args.argc != 2 ||
        fiber::script::js_value_type(args.args[0]) != JsNodeType::Integer ||
        fiber::script::js_value_type(args.args[1]) != JsNodeType::Integer) {
        return AbiResult::exception(
                JsValue::make_exception(
                        fiber::script::ExceptionKind::TypeError));
    }

    return AbiResult::success(JsValue::make_integer(
            fiber::script::js_value_int64(args.args[0]) +
            fiber::script::js_value_int64(args.args[1])));
}

} // namespace

void register_host_functions(
        fiber::script::std_lib::StdLibrary &library) {
    using Signature = fiber::script::Library::FunctionSignature;
    library.register_func(
            "host.add",
            Signature{.required_argc = 2,
                      .fixed_argc = 2,
                      .variadic = false},
            &add,
            nullptr,
            "host.add");
}
```

The script call is simply:

```javascript
return host.add(20, 22);
```

The host ABI exposes:

```cpp
struct HostCallFrame {
    GcHeap &runtime; // Heap for this execution.
    JsValue root;    // Root value for this execution.
    void *attach;    // Host context supplied to exec_*().
};

struct Arguments {
    ConstValueHandle args;
    std::uint32_t argc;
};
```

Arguments have already passed overload selection, default insertion, and spread expansion. The host must still validate value types and runtime state. Host callbacks must be `noexcept`.

`FunctionSignature` rules are:

- A fixed signature matches `required_argc <= argc <= fixed_argc`.
- A variadic signature matches `argc >= fixed_argc`; the variadic tail begins after fixed arguments.
- Defaults cover only trailing fixed arguments and their count must equal `fixed_argc - required_argc`.
- One signature cannot currently combine defaults with a variadic tail.

Example with a default:

```cpp
library.register_func(
        "host.read",
        Signature{.required_argc = 1,
                  .fixed_argc = 2,
                  .variadic = false},
        {fiber::script::JsValue::make_integer(1000)}, // Default timeout.
        &host_read,
        service_ptr,
        "host.read");
```

`userdata` is non-owning. It, and extension objects returned through it, must outlive every compiled `Script` that references them.

### 7.7 Register asynchronous functions and constants

An asynchronous function returns `fiber::script::AsyncTask`:

```cpp
fiber::script::AsyncTask lookup_async(
        void *userdata,
        const fiber::script::Library::HostCallFrame &frame,
        fiber::script::Library::Arguments args) noexcept {
    auto *service = static_cast<MyAsyncService *>(userdata);
    if (!service || args.argc != 1) {
        co_return fiber::script::AbiResult::abort(
                fiber::script::ScriptAbortReason::InvalidState);
    }

    std::int64_t value = co_await service->lookup(args.args[0]);
    co_return fiber::script::AbiResult::success(
            fiber::script::JsValue::make_integer(value));
}

library.register_async_func(
        "host.lookup",
        Signature{.required_argc = 1,
                  .fixed_argc = 1,
                  .variadic = false},
        &lookup_async,
        &service,
        "host.lookup");
```

If the function uses arguments after suspension, `HostCallFrame` and `Arguments` remain valid for that host call. Do not retain either view after its `AsyncTask` completes.

A synchronous constant is registered under `$namespace/key` and accessed as `$namespace.key`:

```cpp
fiber::script::AbiResult environment_name(
        void *userdata,
        const fiber::script::Library::HostCallFrame &frame) noexcept {
    auto name = *static_cast<const std::string_view *>(userdata);
    auto value = fiber::script::JsValue::make_string(
            frame.runtime, name.data(), name.size());
    if (fiber::script::js_value_type(value) !=
        fiber::script::JsNodeType::String) {
        return fiber::script::AbiResult::abort(
                fiber::script::ScriptAbortReason::OutOfMemory);
    }
    return fiber::script::AbiResult::success(value);
}

library.register_constant(
        "$env/name", &environment_name, &environment, "$env.name");
```

```javascript
return {environment: $env.name};
```

For dynamic name-based function/constant/directive resolution, implement `StdLibrary::ExtOps` and install it with `add_ext_ops()`. Extensions are fallback lookups in registration order; directly registered standard-library names win.

## 8. Embedding HTTP scripts in C++

### 8.1 Initialization and compilation

```cpp
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <fiber/http_script/ConstPackage.h>
#include <fiber/http_script/ExchangeConstExtension.h>
#include <fiber/http_script/HttpScriptLib.h>
#include <fiber/http_script/RouteScriptExtension.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/std/StdLibrary.h>

struct ScriptRuntime {
    fiber::script::std_lib::StdLibrary library;
    fiber::http_script::ExchangeConstExtension exchange_constants;
    fiber::http_script::RouteScriptExtension route_extension;

    ScriptRuntime() {
        fiber::http_script::register_http_functions_to_lib(library);
        library.add_ext_ops(
                &exchange_constants,
                fiber::http_script::ExchangeConstExtension::ops());
        library.add_ext_ops(
                &route_extension,
                fiber::http_script::RouteScriptExtension::ops());
    }
};

std::expected<fiber::script::Script,
              fiber::script::parse::ParseError>
compile_route_script(
        ScriptRuntime &runtime,
        fiber::http_script::ConstPackage::Builder &constants,
        std::string_view source,
        const std::vector<std::string> &path_variables) {
    // Every script in a configuration snapshot shares the builder.
    // CompileScope is active only during this serialized compilation.
    fiber::http_script::RouteScriptExtension::CompileScope scope(
            runtime.route_extension,
            constants,
            path_variables,
            true);
    return fiber::script::compile_script(runtime.library, source);
}

// After every script compiles, call this once and store the package in the
// same immutable snapshot as those scripts:
// auto package = constants.build();
```

Lifetime requirements:

- `ConstPackage::Builder` is mutable only during compilation; no constants can be added after `build()`.
- `ConstPackage` owns userdata referenced by dynamic-constant callables. Store it with the scripts compiled against it and keep it alive at least as long.
- `build()` creates compact entry/bucket arrays partitioned by type, with quadratic-probing hash partitions at at most 50% load. Compile-time deduplication maps, order lists, and temporary callables are not retained; stable references and normalized names are.
- Fixed `ExchangeConstExtension` userdata has static storage. `$req`/`$conn` constructs native values directly and does not depend on package identity.
- `StdLibrary` participates only in compilation, but scripts containing HTTP directives still require the directive definitions owned by `RouteScriptExtension` to outlive the script because compiled upstream-call userdata points to them.
- `CompileScope` mutates temporary extension compilation state, so compilation through a shared extension must be serialized. Execution does not read that mutable state.
- When compiling synchronous templates, disable HTTP directives and reject `contains_async()` afterward.
- Text borrowed into runtime slots, exchange text borrowed by `$req`, and `ScriptConnectionInfo::scheme` must last until execution completes. `prepare_constants()` copies decoded query values into the request pool; first access to `$conn.remote_addr` formats and caches it there.

### 8.2 Per-request execution

```cpp
#include <string_view>
#include <utility>
#include <vector>

#include <fiber/http_script/ScriptExchangeCtx.h>
#include <fiber/script/JsGc.h>
#include <fiber/script/Script.h>
#include <fiber/script/ScriptResult.h>

fiber::async::Task<void>
execute_http_script(
        fiber::http::HttpExchange &exchange,
        fiber::script::Script &script,
        const fiber::http_script::ConstPackage &constants,
        const std::vector<std::pair<std::string_view,
                                    std::string_view>> &path_vars,
        fiber::http_script::HttpScriptServices *services,
        fiber::http_script::ScriptConnectionInfo connection) {
    // This form lets GC objects use the request's BufPool.
    fiber::script::GcHeap heap(exchange.pool());
    fiber::http_script::ScriptExchangeCtx context(
            exchange, heap, connection);

    auto prepared = context.prepare_constants(constants);
    if (!prepared ||
        !context.bind_path_constants(constants, path_vars)) {
        (void) co_await context.write_error_json(
                500, "SCRIPT_CONSTANTS");
        co_return;
    }
    context.set_services(services);

    auto result = co_await script.exec_async(
            fiber::script::JsValue::make_undefined(),
            &context,
            heap);

    // resp.send*() or service.proxyPass() already sent the response.
    if (context.response_header_sent()) {
        co_return;
    }

    // Consume ScriptResult while heap/context are still alive and provide a
    // host fallback response.
    using fiber::script::js_value_exception_kind;
    using fiber::script::js_value_is_heap_ref;
    using fiber::script::js_value_type;
    using fiber::script::JsNodeType;
    using fiber::script::ScriptResultKind;

    switch (result.kind) {
        case ScriptResultKind::Value:
            (void) co_await context.write_json(200, result.value());
            break;
        case ScriptResultKind::Void:
            (void) co_await context.write_empty(204);
            break;
        case ScriptResultKind::Exception: {
            const auto &exception = result.exception();
            if (js_value_type(exception) == JsNodeType::Exception &&
                js_value_is_heap_ref(exception)) {
                (void) co_await context.write_json(500, exception);
            } else if (js_value_type(exception) ==
                       JsNodeType::Exception) {
                (void) co_await context.write_error_json(
                        500,
                        fiber::script::exception_kind_name(
                                js_value_exception_kind(exception)));
            } else {
                // Scripts may throw any value; this host hides its contents.
                (void) co_await context.write_error_json(
                        500, "ScriptException");
            }
            break;
        }
        case ScriptResultKind::Abort:
            (void) co_await context.write_error_json(
                    500,
                    fiber::script::abort_reason_name(
                            result.abort().reason));
            break;
    }
    co_return;
}
```

Consume the result while the local heap and context are still alive; never dereference its heap values after this function returns. `run_script()` in `apps/lite_nginx/src/runtime/ServerLauncher.cpp` is the complete in-tree reference.

Bind `$context` and other host values by their compiled indexes:

```cpp
auto index = constants.find(
        fiber::http_script::ConstType::Context, "cluster");
if (index != fiber::http_script::kInvalidConstIndex) {
    context.bind_constant(index, cluster);
}
```

A group of `IndexedConstValue` entries can instead be supplied to `prepare_constants()` or `bind_constants()`. Unset dynamic slots remain `null`. A dynamic-constant callable checks only package identity/index and returns the slot; it does not rescan request data by name. `$req`/`$conn` read directly through `ExchangeConstExtension` and consume no dynamic slot.

### 8.3 Providing upstream connection services

`service.request()` and `service.proxyPass()` depend on a host implementation of `HttpScriptServices`:

```cpp
class HttpScriptServices {
public:
    virtual ~HttpScriptServices() = default;

    virtual fiber::async::Task<fiber::common::IoResult<
            std::unique_ptr<
                    fiber::http_script::HttpUpstreamConnection>>>
    acquire(const fiber::http_script::HttpTargetSpec &target,
            std::chrono::milliseconds connect_timeout) noexcept = 0;
};
```

`acquire()` must return a holder for an already connected HTTP/1 upstream connection. The holder lifetime represents the lease: destroying it should return a reusable connection to its pool or release a transient one. Named-upstream selection, DNS, TLS/SNI, connection-pool keys, and connection establishment are host responsibilities.

lite-nginx provides an `UpstreamRegistry + ConnectionPool + DnsService` implementation in `apps/lite_nginx/src/runtime/HttpScriptServices.*` that custom applications can use as a design reference.

`services` may be null when a script has no upstream directive call. Calling `service.request()`/`service.proxyPass()` without it aborts with `InvalidState`.

## 9. Frequently asked questions and constraints

### Why is there no `await` in scripts?

Asynchrony is part of host-function registration. The compiler emits an asynchronous opcode and the VM suspends/resumes automatically at that point. Source syntax remains synchronous, while C++ must call `exec_async()`.

### Why does a function call fail during compilation?

Function names and arities are statically resolved. Use the intended `Library`, and complete all `register_*`/`add_ext_ops()` calls before compilation. HTTP functions additionally require `register_http_functions_to_lib()`.

### Why does `req.*` produce `InvalidState`?

The execution did not pass the current `ScriptExchangeCtx*` as `attach`, or that context is no longer alive. `req.*`/`resp.*` do not discover a request through global state.

### Why does `exec_sync()` panic?

The script contains an asynchronous opcode. Check `contains_async()` after compilation, use `exec_async()` for request scripts, and reject asynchronous read-only templates.

### Why was the response omitted or sent twice?

After execution, first check `ScriptExchangeCtx::response_header_sent()`. If true, `resp.send*()` or `service.proxyPass()` already sent it and the host must not add a fallback. If false, the host must translate `ScriptResult` into a response.

### Why does `$path.name` fail compilation?

The current route compilation context did not declare that variable. Extract path-variable names from the route pattern, create `RouteScriptExtension::CompileScope` with them, then compile. This check is intentionally compile-time.

### Why is an upstream `url` rejected?

Bind the upstream host in `directive service = http "...";`. The `url` field in `service.request({url: ...})` and `service.proxyPass({url: ...})` accepts only the request `path?query`.

### What are the threading rules?

- Do not concurrently mutate one `StdLibrary` or extension compilation context.
- Do not concurrently use one `GcHeap`/`ScriptExchangeCtx`.
- Compiled `Script` bytecode is read-only, but cross-thread sharing also requires every function's userdata and extension state to satisfy the host's thread-safety model.
- One `GcHeap` and `ScriptExchangeCtx` per request is the simplest ownership model.

## 10. Source and examples

- Compilation: `include/fiber/script/ScriptCompiler.h`
- Execution: `include/fiber/script/Script.h`, `include/fiber/script/ScriptResult.h`
- C++ values and GC: `include/fiber/script/JsValue.h`, `include/fiber/script/JsGc.h`
- Standard-library registration: `src/script/std/StdLibrary.cpp`
- HTTP functions and fixed constants: `src/http_script/RequestFuncs.cpp`, `ResponseFuncs.cpp`, `ExchangeConstExtension.cpp`
- Upstream HTTP: `src/http_script/HttpClientFuncs.cpp`
- HTTP execution context: `include/fiber/http_script/ScriptExchangeCtx.h`
- lite-nginx scripts: `apps/lite_nginx/conf/scripts/`
- Complete HTTP execution reference: `apps/lite_nginx/src/runtime/ServerLauncher.cpp`
- Standard-library behavior tests: `tests/*FuncsTest.cpp`
- HTTP API behavior tests: `tests/HttpScriptFuncsTest.cpp`
