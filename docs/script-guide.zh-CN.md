# Script 模块使用指南

[English](script-guide.md) | 简体中文

本文介绍 `fiber_lib` 内嵌脚本模块的实际用法，包括脚本语言、标准库、C++ 嵌入、HTTP 请求/响应函数、路由变量和脚本发起上游 HTTP 请求的 API。

文中的 TypeScript 仅用于描述脚本 API 的参数和返回值；脚本运行时不是 TypeScript/JavaScript 引擎，也不读取 `.d.ts` 文件。本文以当前 `src/script/` 和 `src/http_script/` 实现为准。

## 1. 模块概览

脚本模块是一套面向网关配置和请求处理的轻量 JS-like 字节码解释器：

- `src/script/parse/`：分词、语法解析和模板字符串解析。
- `src/script/ir/`：把 AST 编译成字节码。
- `src/script/run/`：同步/异步字节码解释器。
- `src/script/gc/`：脚本值、对象、数组、字符串和二进制的 GC 堆。
- `src/script/std/`：默认标准库。
- `src/http_script/`：把 `HttpExchange` 暴露成 `req.*`、`resp.*`、请求常量和上游 HTTP 指令。

编译和执行是分离的。推荐在配置加载阶段编译一次，在请求阶段重复执行已经编译的 `Script`。一次执行必须有自己的 `GcHeap`；`Script` 本身持有只读的编译结果，可以由宿主按自身并发模型共享。

脚本中的异步调用不使用 `await`。函数是否异步在编译时由 `Library` 决定，调用仍写成普通函数调用：

```javascript
let body = req.readJson();
resp.sendJson(200, body);
```

包含异步调用的脚本必须通过 C++ 的 `Script::exec_async()` 执行。

## 2. 快速开始

一个完整的脚本可以读取宿主传入的根对象 `$`，执行计算并返回值：

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

HTTP 脚本通常直接发送响应：

```javascript
let query = req.getQuery();

resp.setHeader("X-Handled-By", "fiber-script");
resp.sendJson(200, {
    method: req.getMethod(),
    path: req.getPath(),
    page: query.page
});
```

## 3. 数据类型

可以用以下 TypeScript 类型近似描述脚本值：

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

需要注意以下差异：

- 数字在运行时分为有符号 64 位整数和 `double`，脚本层统一表现为 `number`。
- `undefined` 表示不存在的属性、未初始化变量或某些 API 的“无值”结果；它不同于 `null`。
- 字符串内部采用 WTF-8/UTF-16 语义。`length()`、字符串索引和 `strings.substring()` 的位置单位是 UTF-16 code unit，不是 UTF-8 字节数，也不是 Unicode code point 数。
- `Binary` 是独立的原始字节类型，不能在脚本中直接写二进制字面量，通常由 `req.readBinary()`、`binary.*` 或 C++ 宿主创建。
- 数组和对象是可变引用值。标准库中的 `array.push()`、`array.pop()`、`Object.assign()` 和 `Object.deleteProperties()` 会原地修改传入值。
- `typeof` 返回模块自己的类型名：`"undefined"`、`"null"`、`"boolean"`、`"number"`、`"string"`、`"binary"`、`"array"`、`"object"`、`"iterator"` 或 `"exception"`。

### 3.1 真值规则

`if`、`!`、`&&`、`||` 和条件表达式使用同一套真值规则：

| 值 | 是否为真 |
| --- | --- |
| `undefined`、`null` | 否 |
| `false` | 否 |
| `true` | 是 |
| 整数 `0`、浮点 `0.0`、`-0.0`、`NaN` | 否 |
| 其他数字，包括正负无穷 | 是 |
| 空字符串 | 否 |
| 非空字符串 | 是 |
| `Binary`、数组、对象、迭代器和异常值 | 是，即使容器为空 |

脚本没有可直接调用的通用“转布尔”函数。需要布尔结果时可以使用 `!!value`。

## 4. 支持的脚本语法

本节描述当前解析器和 VM 的完整脚本语法。它看起来像 JavaScript，但不是 ECMAScript 的子集实现；不要依赖本节未列出的 JavaScript 行为。

### 4.1 空白、注释、标识符和分号

普通空格、制表符、CR 和 LF 会被忽略；源代码中的独立 U+2028/U+2029 不是通用空白。支持两种注释：

```javascript
// 到行末的注释
/* 可以跨行、但不能嵌套的块注释 */
```

建议标识符只使用 `[A-Za-z_$][A-Za-z0-9_$]*`。关键字 `let`、`if`、`else`、`for`、`of`、`continue`、`break`、`return`、`directive`、`try`、`catch` 和 `throw` 不能作为变量名。`true`、`false`、`null`、`typeof` 和 `in` 在对应语法位置也有特殊含义，因此同样不应作为变量名。

分号规则不是 JavaScript ASI：

- `let` 和 `directive` 声明必须以 `;` 结束。
- 表达式、`return`、`throw`、`break` 和 `continue` 后的 `;` 可省略，但建议始终保留。
- 多余的空语句 `;` 会被忽略。
- 换行不会让 `return` 自动结束；`return\nvalue;` 仍会返回 `value`。
- 空脚本是编译错误，空的 `{}` 块合法。

### 4.2 标量字面量

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

- 十进制和十六进制整数存为有符号 64 位整数；`L`/`l` 后缀只影响词法分类，不产生另一种运行时类型。
- 小数、指数形式以及 `F`/`f`/`D`/`d` 后缀都存为 `double`。负数由一元 `-` 组成，不是字面量的一部分。
- 不支持二进制字面量、数字分隔符、BigInt、`NaN`/`Infinity` 字面量或独立的 `undefined` 字面量。
- 整数字面量越过 `int64_t`、或实数字面量超出解析范围，会在编译期报错。

### 4.3 字符串和转义

单引号和双引号字符串等价：

```javascript
let a = "line\ntext";
let b = 'quote: \'';
let c = "\x41\u4e2d";
```

支持 `\\`、`\'`、`\"`、`` \` ``、`\b`、`\f`、`\n`、`\r`、`\t`、`\v`、`\xHH`、`\uHHHH` 和 1 到 3 位八进制转义。反斜杠后紧跟行终止符表示续行且不写入字符。兼容转义 `\a` 当前写入普通字符 `a`，不是响铃字符。普通字符串不能包含未转义换行，也不支持 `\u{...}`。

字符串位置采用 UTF-16 code unit：`"😀".length` 为 2，`"😀"[0]` 与 `"😀"[1]` 分别返回一个代理项字符串。内部 WTF-8 表示可以无损保存这些代理项。

### 4.4 变量、作用域和根值

```javascript
let value;
value = 1;

{
    let value = 2; // 遮蔽外层 value
}

return $.request.id;
```

- `let name;` 初始化为 `undefined`。
- 作用域由脚本、显式块、`if`/`else` 块、循环和 `catch` 块建立。内层声明可以遮蔽外层变量。
- 同一作用域重复 `let` 不报错，而是复用同一个槽；后一个声明会重新赋值。
- 读取尚未声明的普通标识符会创建一个值为 `undefined` 的隐式槽，不会产生 `ReferenceError`；向它赋值也合法。为了可读性，生产脚本仍应显式声明变量。
- C++ 传给 `exec_sync()`/`exec_async()` 的 `root` 参数通过特殊标识符 `$` 读取。`$.user.id` 是普通属性访问；`$query.name` 则是宿主注册的常量，两者不是一回事。

### 4.5 数组和对象字面量

```javascript
let name = "fiber";
let base = {a: 1};
let list = [0, 1, 2,];
let object = {
    name,                    // 简写属性
    "static-key": 1,        // 字符串键
    ["dynamic-" + name]: 2, // 计算键
    ...base,                 // 对象展开
};
```

数组和对象都允许尾逗号，但不支持数组空洞。静态对象键必须是标识符或字符串；同一个对象字面量中的重复静态键是编译错误。计算键必须在运行时得到字符串，否则抛 `TypeError`；多个计算键可以覆盖同一属性。

展开规则如下：

```javascript
let list = [0, ...[1, 2], 3];
let values = [...{a: 1, b: 2}]; // [1, 2]，按属性插入顺序
let object = {a: 1, ...{a: 2, b: 3}};
```

- 数组展开接受数组或对象；对象展开时追加属性值，不追加键。其他类型抛 `TypeError`。
- 对象展开只接受对象，按插入顺序复制属性；其他类型抛 `TypeError`。
- 展开和字面量都创建新容器，但元素/属性值是浅复制。展开不会深拷贝嵌套数组和对象。
- 后写入的属性覆盖先前值；覆盖已有键不会改变其插入位置。

### 4.6 成员访问、索引和赋值

```javascript
let first = list[0];
let name = object.name;
let dynamic = object["dynamic-key"];

object.name = "gateway";
object["new-key"] = 2;
list[0] = 9;
```

读取规则：

- 对象的点访问等价于字符串键索引。缺少属性返回 `undefined`；对 `null`、`undefined` 或其他标量继续读取也返回 `undefined`，不会抛异常。
- 数组索引必须是非负整数且在范围内，否则返回 `undefined`。浮点数 `0.0` 不是有效数组索引。
- 字符串索引必须是整数，返回对应 UTF-16 code unit 的字符串；越界返回 `undefined`。
- 数组和字符串的点属性读取返回其长度。规范用法是 `.length`；当前实现对任意点属性名都返回长度，不应依赖这一兼容细节。

写入规则：

- 变量、对象属性和数组/对象索引可作为赋值左值。赋值表达式返回右值，并且右结合，因此 `a = b = 1` 合法。
- 对象键必须是字符串。点赋值和字符串索引赋值可创建新属性；失败时抛 `TypeError`。
- 数组写入只接受整数已有下标。负数或越界抛 `RangeError`，非整数键抛 `TypeError`；追加应使用 `array.push()`。
- 不能给字符串、`null`、`undefined` 或其他标量写属性/索引；这会抛 `TypeError`。
- `compile_script(..., allow_assign=false)` 会在解析时禁止所有赋值表达式。

### 4.7 函数调用和参数展开

函数调用是编译期静态名称解析：

```javascript
array.push(items, 1, 2);
array.push(items, ...moreItems);
Object.assign(target, ...sources);
```

- `array.push(a, x)` 是完整注册名；`a.push(x)` 不是动态方法调用。
- 函数不是脚本值，不能赋给变量、作为回调传递或动态调用。
- 参数从左到右求值。参数展开接受数组或对象；对象按插入顺序展开属性值。其他类型抛 `TypeError`。
- 含展开参数的调用在编译期被视为参数数量未知，因此只能匹配可变参数签名。固定参数函数即使运行时展开长度恰好匹配，也会编译失败。
- 未知函数、参数数量不匹配、同步/异步重载冲突或重载歧义都在编译期报错。
- 异步函数在源码中仍按普通调用书写。编译结果会记录异步 opcode，必须用 `exec_async()` 执行。

### 4.8 运算符优先级

从高到低：

| 优先级 | 运算符 | 结合性 |
| --- | --- | --- |
| 1 | `()`、函数调用、`.`、`[]` | 从左到右 |
| 2 | 一元 `+`、`-`、`!`、`typeof` | 从右到左 |
| 3 | `*`、`/`、`%` | 从左到右 |
| 4 | `+`、`-` | 从左到右 |
| 5 | `<`、`<=`、`>`、`>=`、`==`、`!=`、`===`、`!==`、`in`、`~` | 从左到右 |
| 6 | `&&` | 从左到右，短路 |
| 7 | `||` | 从左到右，短路 |
| 8 | `?:`、`=` | 从右到左 |

所有比较与相等运算符处在同一级，这一点不同于 JavaScript。复杂比较请加括号。`~` 可被解析，但当前编译器明确拒绝它，见 4.17。

### 4.9 算术和一元运算

数字运算接受整数、浮点数、布尔值和 `null`；布尔值转为 0/1，`null` 转为 0。普通字符串不会隐式转数字。

- 一元 `+` 返回数值化结果；一元 `-` 取负；非数值兼容类型抛 `TypeError`。对 `INT64_MIN` 取负会提升为浮点数。
- `+` 在任一操作数是字符串时执行拼接。另一侧可为 `undefined`、`null`、布尔值、数字或字符串，分别使用兼容文本；数组、对象和 `Binary` 会导致 `TypeError`。
- 没有字符串操作数时，`+`、`-`、`*` 执行数值运算。整数溢出时结果提升为 `double`；任一操作数是浮点数时结果也是浮点数。
- `/` 总是返回浮点数。除数为整数或浮点 0 时抛 `RangeError`。
- `%` 对整数返回整数余数，对含浮点操作数的表达式使用浮点余数；除数为 0 时抛 `RangeError`。
- `!value` 总是返回布尔值，规则见 3.1；`typeof value` 总是返回第 3 节列出的类型字符串。

这些运算除结果分配外没有副作用。

### 4.10 比较、相等、逻辑和条件运算

- `<`、`<=`、`>`、`>=`：两个字符串按 UTF-16 code unit 字典序比较；两个数值兼容值转换为 `double` 比较。其他组合（包括字符串与数字混合）直接返回 `false`，不抛异常。
- `===`/`!==`：数字统一转成 `double` 后比较，因此整数/浮点表示可跨表示比较，但绝对值超过 `2^53` 的不同整数可能因精度折叠而相等；字符串按内容比较；数组、对象、二进制、迭代器和异常按身份比较；`NaN` 不等于自身。
- `==`/`!=`：在严格比较之外，`null == undefined`；布尔值按 0/1 与数字或数值字符串比较；数字和字符串按完整数值文本转换后比较。该转换忽略两端 ASCII 空白，空文本为 0，并接受十进制/指数、`0x` 十六进制（含小数和 `p` 指数）以及大小写不敏感的 `NaN`、`Inf`/`Infinity`；其他尾随字符使比较不相等。对象、数组和二进制不做字符串或原始值转换。
- `key in object` 只接受字符串键；`index in array` 只接受非负整数下标。类型不匹配或不存在时返回 `false`。
- `left && right` 在 `left` 为假时返回 `left`，否则求值并返回 `right`。`left || right` 在 `left` 为真时返回 `left`，否则求值并返回 `right`。
- `condition ? whenTrue : whenFalse` 只求值一个分支并返回该分支的值。

### 4.11 `if`、块和表达式语句

`if` 和 `else` 分支必须使用花括号；支持 `else if`：

```javascript
if ($.status >= 500) {
    return "server-error";
} else if ($.status >= 400) {
    return "client-error";
} else {
    return "ok";
}
```

条件使用 3.1 的真值规则。独立表达式可以作为语句，结果会被丢弃；函数副作用调用和赋值通常以这种形式出现。

### 4.12 `for-of`、`break` 和 `continue`

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

`for` 只支持以下双变量 `for-of` 形式：

```typescript
// 数组：key 为从 0 开始的下标，value 为元素。
for (let key, value of array) { /* ... */ }

// 对象：key 为属性名，value 为属性值，按插入顺序迭代。
for (let key, value of object) { /* ... */ }
```

- 集合表达式只求值一次。数组和对象之外的值被视为空集合，不抛异常。
- 循环变量只在该循环体中可见，每次迭代覆盖其值。
- `continue` 跳到下一次迭代，`break` 退出最内层循环。当前编译器会把循环外的 `break`/`continue` 编译为空操作，但这是实现兼容细节，不应使用。
- 迭代期间新增/删除数组元素或对象属性属于结构修改，会抛可捕获的 `IterationError`；更新已有数组元素或对象属性的值不属于结构修改。

### 4.13 `return`

```javascript
return expression; // ScriptResultKind::Value，包括 expression 为 undefined
return;            // ScriptResultKind::Void
```

运行到脚本末尾也得到 `Void`。`return` 可以出现在嵌套块、循环或 `try/catch` 中，并立即结束整个脚本。

### 4.14 `throw`、`try` 和 `catch`

```javascript
try {
    let value = JSON.parse($.input);
    return value;
} catch (error) {
    return {ok: false, error};
}
```

`catch` 必须写绑定变量和花括号；不支持省略绑定或 `finally`。脚本可用 `throw value;` 抛出任意值。标准库的类型、范围和解析错误也可以被 `try/catch` 捕获，捕获变量就是原始异常值。

内建的轻量异常包括 `TypeError`、`RangeError`、`ReferenceError` 和 `IterationError`。部分解析与 HTTP API 会产生带名称/消息的堆异常，例如 `SyntaxError` 或 `Error`。脚本当前没有读取异常名称、消息或位置的专用函数；宿主可通过 C++ GC API 检查堆异常。

C++ 结果中的 `Exception` 和 `Abort` 必须区别处理：

- `Exception` 是脚本语义错误，可被脚本内 `try/catch` 捕获；未捕获时成为 `ScriptResultKind::Exception`。
- `Abort` 是运行时终止，例如 `OutOfMemory`、`InvalidState`、`InvalidOpcode`、`HostFault`、`Timeout`、`Cancelled` 或 `Internal`，不能被脚本捕获。

如果一个会原地修改容器的函数在中途因 `OutOfMemory` 中止，已经完成的修改不保证回滚。

### 4.15 模板字符串

脚本内支持反引号模板字符串：

```javascript
let id = 42;
return `item-${id}-${1 + 2}`;
```

模板可跨行并支持普通字符串的大多数转义，另外 `\$` 写入字面 `$`。`${expression}` 中可以嵌套字符串、对象和模板；每个表达式按 `+` 的原始值转字符串规则拼接，因此插值数组、对象或二进制会抛 `TypeError`。空模板返回空字符串。

C++ 还提供 `compile_template_string()`，输入是“不带外层反引号”的模板正文：

```text
prefix-${$.name}-${length($.items)}
```

模板始终返回字符串。`compile_template_string()` 默认禁止赋值，但不会自动禁止异步函数；准备通过 `exec_sync()` 执行模板时，宿主必须检查 `Script::contains_async()`。

### 4.16 `directive`

`directive` 是宿主扩展的编译期绑定，不是运行时变量声明：

```javascript
directive backend = http "@api";
// 等价的通用语法别名：directive backend from http "@api";

return backend.request({path: "/health"});
```

- 形式为 `directive name = type literal...;` 或 `directive name from type literal...;`。参数只能是 `null`、布尔、数字或字符串字面量，字面量之间不写逗号。
- directive 名在整个编译单元中唯一，并且必须先声明、后调用。
- 解析器把声明交给 `Library::resolve_directive_def()`；宿主不认识该类型、名称或字面量组合时编译失败。
- 声明本身不生成运行时指令。directive 方法仍由宿主在编译期静态解析。
- HTTP 扩展只接受一个字符串目标，具体见 6.4。

### 4.17 明确不支持的语法

当前不支持 `const`/`var`、`while`/`do`、传统三段式 `for`、单变量 `for-of`、`switch`、函数或箭头函数、类、`new`、可选链、空值合并、复合赋值、`++`/`--`、位运算、正则字面量、模块、Promise 或 `await`。

词法器会识别 `++`、`--`、`~`、`#[` 和 `^[` 的部分 token，但解析器/编译器没有提供对应可执行语义；使用它们会编译失败。`strings.match` 和 `strings.findAll` 也尚未注册。

## 5. 标准库 API

`fiber::script::std_lib::StdLibrary` 构造时注册本节列出的 45 个调用名（共 46 个重载）。这里的“异常”指脚本可捕获的 `Exception`；“中止”指不可捕获的 `Abort`。

所有函数共同遵循以下规则：

- 名称、重载和参数数量在编译期解析；数量错误不会进入函数体。
- 除明确写为原地修改外，函数不修改参数。返回的新字符串、二进制、数组或对象属于当前 `GcHeap`。
- 任何需要分配结果的函数都可能因分配失败产生 `OutOfMemory` abort；内部状态缺失还可能产生 `InvalidState` abort。下面不在每一行重复这两个通用中止。
- 可变参数函数发生中途 `OutOfMemory` 时不提供事务回滚保证。

### 5.1 通用函数

#### `length(value?)`

```typescript
function length(value?: ScriptValue): number;
```

- 参数：`value` 缺省为 `null`。
- 返回：字符串的 UTF-16 code unit 数、`Binary` 的字节数、数组元素数或对象属性数；其他类型返回 `0`。宿主提供的借用字符串若不是合法 UTF-8，则长度回退为字节数。
- 异常：无。
- 副作用：无。

#### `includes(container, ...items)`

```typescript
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

- 参数：字符串容器要求每个 `item` 也是字符串；数组容器接受任意脚本值。
- 返回：字符串中包含所有子串，或数组中按 `===` 分别找到所有元素时为 `true`。容器不是字符串/数组时为 `false`；没有 `items` 时，合法容器为 `true`。
- 异常：无；类型不匹配返回 `false`。
- 副作用：无。

### 5.2 数组函数

#### `array.join(values, separator?)`

```typescript
function array.join(values: ScriptValue[], separator?: ScriptValue): string | undefined;
```

- 参数：`values` 必须是数组；`separator` 默认空字符串。字符串、数字和布尔值按文本表示；`null`、`undefined`、容器、迭代器、异常和二进制按空文本处理。
- 返回：按转换后的分隔符连接元素的新字符串。默认分隔符不是 JavaScript 的逗号；当前实现若最后的 GC 字符串分配失败会返回 `undefined`。
- 异常：`values` 非数组时抛 `TypeError`。
- 副作用：无。

#### `array.pop(values)`

```typescript
function array.pop(values: ScriptValue[]): ScriptValue | null;
```

- 参数：`values` 必须是数组。
- 返回：被删除的最后一个元素；空数组返回 `null`。
- 异常：非数组抛 `TypeError`。
- 副作用：原地缩短数组，属于结构修改；迭代同一数组时调用会触发后续 `IterationError`。

#### `array.push(values, ...items)`

```typescript
function array.push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
```

- 参数：`values` 必须是数组；`items` 可以为空。
- 返回：同一个 `values` 数组，不是新长度。
- 异常：非数组抛 `TypeError`。
- 副作用：按顺序原地追加元素，属于结构修改；分配中止前已经追加的元素不会回滚。

### 5.3 字符串函数

字符串 API 的软失败是有意的：除非下文明确写出异常，文本参数类型错误时返回 `false` 或 `null`，不会抛 `TypeError`。这些函数要求能取得严格 UTF-8 view；含孤立 UTF-16 代理项的 WTF-8 字符串也按文本类型失败处理，但语言本身的索引、拼接和 `length()` 仍能处理它。

#### `strings.hasPrefix(text, prefix)` / `strings.hasSuffix(text, suffix)`

```typescript
function strings.hasPrefix(text: string, prefix: string): boolean;
function strings.hasSuffix(text: string, suffix: string): boolean;
```

- 参数：两个参数都必须是字符串；空前缀/后缀合法。
- 返回：按字节等价的 Unicode 文本前缀/后缀判断；任一参数非字符串时返回 `false`。
- 异常：无。
- 副作用：无。

#### `strings.toLower(text)` / `strings.toUpper(text)`

```typescript
function strings.toLower(text: string): string | null;
function strings.toUpper(text: string): string | null;
```

- 参数：`text` 必须是字符串。
- 返回：只转换 ASCII `A-Z`/`a-z` 的新字符串；非 ASCII 字符原样保留。非字符串返回 `null`。
- 异常：无。
- 副作用：无。

#### `strings.trim(text, cutset?)`

```typescript
function strings.trim(text: string, cutset?: string | null): string | null;
```

- 参数：`text` 必须是字符串。省略 `cutset`，或传入非字符串/`null`，使用默认模式；字符串 `cutset` 被视为一个完整子串，不是字符集合。
- 返回：默认模式删除两端字节值 `<= 0x20` 的字符；指定 `cutset` 时从两端反复删除完整的 `cutset`。空 `cutset` 不删除内容。`text` 非字符串返回 `null`。
- 异常：无。
- 副作用：无。

#### `strings.trimLeft(text, cutset?)` / `strings.trimRight(text, cutset?)`

```typescript
function strings.trimLeft(text: string, cutset?: string | null): string | null;
function strings.trimRight(text: string, cutset?: string | null): string | null;
```

- 参数：规则与 `trim()` 相同。
- 返回：只处理对应一端。默认 whitespace 是 ASCII `0x09-0x0d`、`0x1c-0x20`；指定 `cutset` 时反复删除完整子串。`text` 非字符串返回 `null`。
- 异常：无。
- 副作用：无。

#### `strings.split(text, separators?)`

```typescript
function strings.split(text: string, separators?: string | null): string[] | null;
```

- 参数：`text` 必须是字符串。省略 `separators`，或传入非字符串/`null`，表示不拆分；字符串 `separators` 是 Unicode code point 集合，其中任一字符都可作为分隔符。
- 返回：未指定分隔符时返回只含原始 `text` 的新数组。指定分隔符时，连续、开头和结尾的分隔符不产生空元素；空文本返回空数组。`text` 非字符串返回 `null`。
- 异常：无。
- 副作用：无。

#### `strings.contains(text, value)` / `strings.contains_any(text, chars)`

```typescript
function strings.contains(text: string, value: string): boolean | null;
function strings.contains_any(text: string, chars: string): boolean | null;
```

- 参数：`contains` 把 `value` 当完整子串；`contains_any` 把 `chars` 当 code point 集合。
- 返回：找到时为 `true`，否则为 `false`；任一参数非字符串时返回 `null`。空子串被所有字符串包含，空字符集合不匹配任何字符。
- 异常：无。
- 副作用：无。

#### `strings.index(text, value)` / `strings.lastIndex(text, value)`

```typescript
function strings.index(text: string, value: string): number | null;
function strings.lastIndex(text: string, value: string): number | null;
```

- 参数：两个参数都必须是字符串；`value` 是完整子串。
- 返回：第一次/最后一次匹配的 UTF-16 起始下标，找不到为 `-1`；类型错误为 `null`。
- 异常：无。
- 副作用：无。

#### `strings.indexAny(text, chars)` / `strings.lastIndexAny(text, chars)`

```typescript
function strings.indexAny(text: string, chars: string): number | null;
function strings.lastIndexAny(text: string, chars: string): number | null;
```

- 参数：`chars` 是 Unicode code point 集合。
- 返回：`indexAny` 返回 `text` 中任一候选字符的第一个 UTF-16 下标。`lastIndexAny` 按 `chars` 的字符顺序查找，返回“第一个确实出现的候选字符”在 `text` 中的最后位置，并非所有候选位置的全局最大值。找不到为 `-1`，类型错误为 `null`。
- 异常：无。
- 副作用：无。

#### `strings.repeat(text, count)`

```typescript
function strings.repeat(text: string, count: number): string | null;
```

- 参数：`text` 必须是字符串；`count` 必须是整数或浮点数，浮点数向零截断后按 32 位计数使用。`NaN` 按 0；负数和非数字返回 `null`。
- 返回：重复后的字符串；0 次或空输入返回空字符串，1 次可以返回原字符串值。
- 异常/中止：无可捕获异常；结果超过 16 MiB 时产生 `OutOfMemory` abort。
- 副作用：无。

#### `strings.substring(text, start?, end?)`

```typescript
function strings.substring(text: string, start?: ScriptValue, end?: ScriptValue): string | null;
```

- 参数：`text` 必须是字符串。`start`/`end` 默认 0/`2147483647`，单位为 UTF-16 code unit。整数直接使用，浮点向零截断，布尔值按 0/1，`null`/`undefined`/不可转换值按 0；字符串解析其开头的十进制整数，失败按 0。
- 返回：`start < 0` 时从 0 开始；`start >= length` 或 `end <= start` 时为空字符串；`end` 超过长度时截到末尾。边界若落在补充字符代理项对的中间，当前实现会回退到该 code point 的起始位置，不会像字符串索引那样生成孤立代理项。`text` 非字符串返回 `null`。
- 异常：无。
- 副作用：无。

#### `strings.toString()` / `strings.toString(value)`

```typescript
function strings.toString(): string;
function strings.toString(value: ScriptValue): string;
```

- 参数：0 或 1 个参数是两个独立重载。
- 返回：无参数为空字符串；`null`/`undefined` 为 `"null"`；标量为兼容文本；数组/对象为 `"<ArrayNode>"`/`"<ObjectNode>"`；`Binary` 把原始字节作为文本内容。它不是 JSON 序列化。
- 异常：无；无法形成结果时可能按通用规则中止。
- 副作用：无。

`strings.match` 和 `strings.findAll` 当前没有注册。

### 5.4 二进制函数

#### `binary.base64Encode(value)`

```typescript
function binary.base64Encode(value: Binary): string | undefined;
```

- 参数/返回：`Binary` 编码为标准 Base64 字符串；其他类型返回 `undefined`。空二进制返回空字符串。
- 异常：无。
- 副作用：无。

#### `binary.base64Decode(value)`

```typescript
function binary.base64Decode(value: string): Binary | undefined;
```

- 参数/返回：字符串按严格标准 Base64 解码成新 `Binary`；非字符串返回 `undefined`。
- 异常：非法字符、空白、错误填充或非 4 倍数长度抛 `RangeError`。
- 副作用：无。

#### `binary.hex(value)`

```typescript
function binary.hex(value: Binary): string;
```

- 参数/返回：把二进制编码为小写十六进制字符串；空二进制返回空字符串。
- 异常：非二进制抛 `TypeError`。
- 副作用：无。

#### `binary.fromHex(value)`

```typescript
function binary.fromHex(value: string): Binary;
```

- 参数/返回：严格解码偶数长度、大小写均可的十六进制字符串，返回新 `Binary`。
- 异常：非字符串抛 `TypeError`；奇数长度或非法字符抛 `RangeError`。
- 副作用：无。

#### `binary.getUtf8Bytes(value)`

```typescript
function binary.getUtf8Bytes(value: ScriptValue): Binary;
```

- 参数/返回：把兼容文本表示复制成二进制。字符串为 UTF-8 字节，标量为其文本，`null` 为 `null`，数组/对象为 `<ArrayNode>`/`<ObjectNode>`，`Binary` 为原始字节，`undefined`/迭代器/异常为空。
- 异常：无。
- 副作用：无；返回值始终是新的二进制值。

### 5.5 哈希函数

#### `hash.crc32(value)`

```typescript
function hash.crc32(value: ScriptValue): number;
```

- 参数：使用兼容标量文本；`null` 是 `"null"`，数组、对象、二进制、`undefined`、迭代器和异常是空文本。
- 返回：CRC-32，范围为 0 到 `0xffffffff`；空文本为 0。
- 异常：无。
- 副作用：无。

#### `hash.md5(value)` / `hash.sha1(value)` / `hash.sha256(value)`

```typescript
function hash.md5(value: string | Binary): string;
function hash.sha1(value: string | Binary): string;
function hash.sha256(value: string | Binary): string;
```

- 参数：字符串使用 UTF-8 字节，`Binary` 使用原始字节。
- 返回：分别为 32、40、64 个小写十六进制字符。
- 异常/中止：其他类型抛 `TypeError`；底层摘要实现异常失败产生 `HostFault` abort。
- 副作用：无。

MD5/SHA-1 仅用于互操作，不应用于需要抗碰撞性的安全设计。

### 5.6 数学和随机函数

#### `math.floor(value)`

```typescript
function math.floor(value: number): number;
```

- 参数/返回：整数原样返回；可表示为 `int64_t` 的有限浮点数向负无穷取整并返回整数。当前实现没有为 `NaN`、无穷或超出 `int64_t` 范围的浮点输入定义可移植结果，调用方应先避免这些值。
- 异常：非数字抛 `TypeError`。
- 副作用：无。

#### `math.abs(value)`

```typescript
function math.abs(value: number): number;
```

- 参数/返回：整数或浮点数的绝对值；浮点 `NaN` 仍是 `NaN`，正负无穷都返回正无穷；为兼容 Java，`INT64_MIN` 保持原负值。
- 异常：非数字抛 `TypeError`。
- 副作用：无。

#### `rand.random(max?)`

```typescript
function rand.random(max?: number): number;
```

- 参数：`max` 默认 1000，必须是数字；浮点数向零截断并饱和到 64 位范围。
- 返回：当前线程 PRNG 产生的均匀整数，范围 `[0, max)`；不提供密码学安全保证，也不能设置种子。
- 异常：非数字抛 `TypeError`；截断后的 `max <= 0` 抛 `RangeError`。
- 副作用：推进当前线程的伪随机状态，因此同一调用不会稳定复现。

#### `rand.canary(ratio, ...keys)`

```typescript
function rand.canary(ratio: ScriptValue, ...keys: ScriptValue[]): boolean;
```

- 参数：数字 ratio 向零截断，布尔值按 0/1，其他类型按 0。`ratio <= 0` 恒假，`ratio >= 100` 恒真。
- 返回：没有 key 时随机选择 `[0, 100)` 桶；有 key 时按参数顺序把非空兼容文本累计进 CRC-32，再用 `% 100` 得到稳定桶。`null` key 的文本是 `"null"`；容器、二进制和 `undefined` 为空并被跳过。key 顺序会影响结果。
- 异常：无。
- 副作用：只有没有 key 且 ratio 在 1..99 时推进随机状态；带 key 的调用是确定性的。

### 5.7 JSON 函数

#### `JSON.parse(text)`

```typescript
function JSON.parse(text: string): ScriptValue;
```

- 参数/返回：解析一个完整 JSON 文档并构造对应脚本值。JSON 数字根据表示和范围成为整数或浮点数；对象保留输入属性顺序，重复 key 以后值覆盖前值但保留第一次出现的位置。
- 异常：非字符串抛 `TypeError`；非法 JSON 抛带 `name = "SyntaxError"`、消息和字节偏移的异常。
- 副作用：无；会在当前堆分配整个结果树。

#### `JSON.stringify(value)`

```typescript
function JSON.stringify(value: ScriptValue): string | undefined;
```

- 参数/返回：顶层 `undefined` 原样返回 `undefined`；顶层 `NaN`/正负无穷返回字符串 `"null"`；其他值返回紧凑 JSON。嵌套 `undefined` 编码为 JSON `null`，`Binary` 编码为 Base64 字符串，异常值编码为含 `position`、`name`、`message`、`meta` 的对象，普通对象按插入顺序输出。
- 异常：无效字符串、嵌套非有限数字、迭代器或其他无法编码的值抛 `TypeError`。
- 副作用：无。

### 5.8 对象函数

#### `Object.assign(target, source, ...sources)`

```typescript
function Object.assign(
    target: ScriptObject,
    source: ScriptValue,
    ...sources: ScriptValue[]
): ScriptObject;
```

- 参数：`target` 必须是对象；非对象 source 被静默跳过。来源按参数顺序、属性按插入顺序处理。
- 返回：同一个 `target`。
- 异常：target 非对象抛 `TypeError`。
- 副作用：原地新增或覆盖属性；覆盖不改变位置。中途 OOM 时已复制字段不回滚。

#### `Object.keys(value)` / `Object.values(value)`

```typescript
function Object.keys(value: ScriptObject): string[];
function Object.values(value: ScriptObject): ScriptValue[];
```

- 参数/返回：对象按属性插入顺序生成新的键数组或值数组；值为浅复制。
- 异常：非对象抛 `TypeError`。
- 副作用：不修改输入对象。

#### `Object.deleteProperties(target, key, ...keys)`

```typescript
function Object.deleteProperties(
    target: ScriptObject,
    key: ScriptValue,
    ...keys: ScriptValue[]
): ScriptObject;
```

- 参数：target 必须是对象；只有字符串 key 生效，非字符串和不存在的 key 被跳过。
- 返回：同一个 `target`。
- 异常：target 非对象抛 `TypeError`。
- 副作用：原地删除属性，属于结构修改；当前底层删除失败会按无操作处理。

### 5.9 URL 表单函数

这些函数实现 `application/x-www-form-urlencoded`，不是 ECMAScript `encodeURIComponent`：空格编码为 `+`，`+` 解码为空格。

#### `URL.encodeComponent(value)`

```typescript
function URL.encodeComponent(value: string): string;
```

- 参数/返回：保留字母、数字、`-`、`_`、`.`、`*`；空格写成 `+`，其他 UTF-8 字节写成大写 `%HH`。
- 异常：非字符串抛 `TypeError`。
- 副作用：无。

#### `URL.decodeComponent(value)`

```typescript
function URL.decodeComponent(value: string): string;
```

- 参数/返回：把 `+` 解码为空格并解码 `%HH`；解码后的非法 UTF-8 字节序列替换为 U+FFFD。
- 异常：非字符串抛 `TypeError`；不完整或非法百分号转义抛 `RangeError`。
- 副作用：无。

#### `URL.parseQuery(value)`

```typescript
function URL.parseQuery(value: string): {
    [key: string]: string | string[];
};
```

- 参数/返回：解析不带 `?` 的查询串。空 `&` 段被跳过；没有 `=` 的字段值为空字符串；值中的后续 `=` 被保留。重复 key 第一次为字符串，第二次提升为数组，之后继续追加；解码后的非法 UTF-8 替换为 U+FFFD。
- 异常：非字符串抛 `TypeError`；任一 key/value 中的非法百分号转义抛 `RangeError`。
- 副作用：无；返回新对象和必要的新数组。

#### `URL.buildQuery(value?)`

```typescript
function URL.buildQuery(
    value?: null | undefined | { [key: string]: ScriptValue | ScriptValue[] }
): string | null | undefined;
```

- 参数：缺省为 `undefined`。对象属性按插入顺序处理；数组值展开为重复 key，空数组不产生字段。值使用兼容文本：`null` 为 `null`，对象/数组为节点占位文本，二进制使用原始字节。
- 返回：form-urlencoded 查询串；`null`/`undefined` 参数原样返回。
- 异常：其他非对象参数抛 `TypeError`。
- 副作用：无。

## 6. HTTP Script API

HTTP API 不是 `StdLibrary` 的默认内容。C++ 宿主必须调用 `register_http_functions_to_lib()`，并在执行时把与当前请求和 `GcHeap` 绑定的 `ScriptExchangeCtx*` 作为 `attach` 参数传入。

HTTP 函数的完整注册名为：

```text
req.getHeader      req.getQuery       req.getCookie       req.getUri
req.getPath        req.getQueryStr    req.getMethod       req.readJson
req.readBinary     req.discardBody
resp.setHeader     resp.addHeader     resp.addCookie      resp.sendJson
resp.send
```

这些函数共有以下约束：

- 缺少有效 `ScriptExchangeCtx` 时产生不可捕获的 `InvalidState` abort；上游函数还要求 context 已配置 `HttpScriptServices`。
- `req.readJson()`、`req.readBinary()`、`req.discardBody()`、`resp.sendJson()`、`resp.send()` 和 directive 的 `request()`/`proxyPass()` 是异步函数，包含它们的脚本必须通过 `exec_async()` 执行。
- HTTP I/O、编码和配置错误通常抛出名为 `Error` 的可捕获异常；构造值或异常失败仍可能产生 `OutOfMemory` abort。
- 网络、请求体和响应状态都不是事务。异常不会撤销已经读取的请求体、已发出的上游请求、已提交的响应头或已写出的响应体。

### 6.1 请求函数 `req.*`

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

    // 异步宿主函数，脚本中仍按普通函数调用。
    function readJson(): ScriptValue;
    function readBinary(): Binary;
    function discardBody(): null;
}
```

#### `req.getHeader()` / `req.getHeader(name)`

- 参数：无参数重载读取全部请求头；单参数重载要求 `name` 是非空字符串，并按 HTTP 头名称规则进行不区分大小写查找。
- 返回：无参数重载延迟构造并缓存一个对象，键沿用请求头字段名且区分大小写；完全相同的重复键以后值覆盖前值，不同大小写的名称可能同时存在；缓存对象分配失败时返回 `undefined`。单参数重载在命中时返回字段值，不存在时返回 `undefined`，名称为空或非字符串时返回 `null`。
- 异常/中止：没有可捕获异常；缺少 context 为 `InvalidState`，结果分配失败为 `OutOfMemory`。
- 副作用：无参数重载会在本次 `ScriptExchangeCtx` 中建立 GC 根并缓存对象；不消费请求体、不修改请求。

#### `req.getQuery()` / `req.getQuery(name)`

- 参数：无参数重载解析完整原始 query；单参数重载要求非空字符串 key，并按解码后的 key 精确、区分大小写查找。
- 返回：无参数重载返回并缓存 form-urlencoded 解码对象，重复 key 保留最后一个值；分配失败时可能返回 `undefined`。单参数重载返回缓存对象中的值，key 不存在、为空或非字符串时返回 `undefined`。
- 异常/中止：非法 `%HH` 当前不抛异常，而是停止解析并保留此前字段；缺少 context 或查找所需 GC 根失败分别为 `InvalidState`/`OutOfMemory` abort。
- 副作用：首次调用会解析并缓存对象；`+` 解码为空格，非法 UTF-8 替换为 U+FFFD；不消费请求体。

#### `req.getCookie()` / `req.getCookie(name)`

- 参数：无参数重载解析所有 `Cookie` 请求头；字段按 `;` 分段、去掉两端空格/Tab，并用第一个 `=` 分开名称和值，没有 `=` 时值为空。它不做百分号解码或引号反转义。单参数重载要求非空字符串 cookie 名，并区分大小写精确查找。
- 返回：无参数重载返回并缓存 cookie 对象，同名 cookie 后值覆盖前值；分配失败时可能返回 `undefined`。单参数重载在不存在、空名称或非字符串时返回 `undefined`。
- 异常/中止：没有 cookie 语法异常；缺少 context 或查找所需 GC 根失败分别为 `InvalidState`/`OutOfMemory` abort。
- 副作用：首次调用会解析并缓存对象，不消费请求体。

#### `req.getUri()` / `req.getPath()` / `req.getQueryStr()` / `req.getMethod()`

| 函数 | 返回值 |
| --- | --- |
| `req.getUri()` | 原始请求目标 `path[?query]`，不含 scheme 和 host |
| `req.getPath()` | 解析后的 path |
| `req.getQueryStr()` | 不含 `?` 的原始 query；没有 query 时为空字符串 |
| `req.getMethod()` | 当前 HTTP 方法名 |

四个函数都不接收参数，不抛可捕获异常，也不修改请求；缺少 context 或字符串分配失败分别产生 `InvalidState`/`OutOfMemory` abort。

#### `req.readJson()`

- 参数：无；异步函数。
- 返回：读取完整请求体并构造对应 `ScriptValue`。
- 异常：空请求体、body 读取失败或非法 JSON 分别抛出带固定消息的 `Error`；解析错误的具体 offset 不会暴露给脚本。
- 副作用：消费请求体，并把完整 body 和解码结果置于内存；函数自身不限制大小。

#### `req.readBinary()`

- 参数：无；异步函数。
- 返回：包含完整请求体的新 `Binary`；空 body 返回长度为 0 的 `Binary`。
- 异常：读取失败抛 `Error`；分配失败为 `OutOfMemory` abort。
- 副作用：消费请求体，并把完整 body 连续化到内存；函数自身不限制大小。

#### `req.discardBody()`

- 参数：无；异步函数。
- 返回：始终为 `null`。
- 异常：当前会忽略底层排空错误，不抛可捕获异常；缺少 context 为 `InvalidState` abort。
- 副作用：消费并丢弃剩余请求体。

请求体是一次性的流式资源。不要把这三个函数相互组合，也不要先读取 body 再调用会转发入站 body 的 `service.proxyPass()`。公网服务还应在 HTTP 层设置请求体大小限制。

### 6.2 响应函数 `resp.*`

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

    // 异步宿主函数。
    function sendJson(status: number, body: ScriptValue): null;
    function send(status: number): null;
    function send(status: number, body: ScriptValue): null;
}
```

三个发送重载都只识别整数 status，其他类型回退为 200；脚本绑定层不预先校验 HTTP 状态码范围，协议层拒绝时按发送 `Error` 处理。

#### `resp.setHeader(name, value)`

- 参数：`name` 必须是非空字符串；`value` 使用标量文本转换，字符串、整数、浮点数、布尔值和 `null` 分别得到自身文本。
- 返回：成功为 `null`。
- 异常：name 非字符串/空字符串，或 value 转换为空文本时抛 `Error`。因此空字符串、`undefined`、数组、对象、`Binary`、迭代器和异常值不能作为 header value。HTTP 名称/值最终不被协议层接受时，也可能在发送阶段抛 `Error`。
- 副作用：响应头尚未发送时，替换所有同名 pending header；发送后静默不做修改，仍返回 `null`。

#### `resp.addHeader(name, value)`

- 参数、返回和异常：与 `resp.setHeader()` 相同。
- 副作用：响应头尚未发送时追加字段，允许多个同名值；发送后静默不做修改。

#### `resp.addCookie(cookie)`

- 参数：必须是对象。`name` 必须是非空、合法 HTTP token 字符串；`value` 使用标量文本转换，缺省或不可转换值成为空字符串；`domain`/`path` 只有字符串才生效；`maxAge` 只有非负整数才输出；`secure`/`httpOnly` 只有布尔 `true` 才输出；`sameSite` 只有大小写完全匹配的 `"Lax"`、`"Strict"` 或 `"None"` 才输出。
- 返回：编码成功为 `true`；参数非对象、name 缺失/类型错误/非法时为 `false`。其他字段类型不符时通常只是忽略该字段，不会让调用失败。
- 异常：没有可捕获异常；缺少 context 为 `InvalidState` abort。
- 副作用：成功时向 pending headers 追加一个 `Set-Cookie`。响应头已经发送后，header 写入会被忽略，但当前函数仍可能返回 `true`；返回值只表示 cookie 编码成功。

#### `resp.sendJson(status, body)`

- 参数：异步函数。`status` 只有整数才生效，其他类型回退为 200；`body` 接受任意可 JSON 编码的脚本值。
- 返回：发送成功为 `null`。`undefined` 编码为 JSON `null`，`Binary` 编码为 Base64 字符串，异常值编码为包含 `position`、`name`、`message` 和 `meta` 的对象。
- 异常：非法字符串、非有限浮点数、迭代器等编码失败，或 header/body I/O 失败时抛 `Error`。
- 副作用：把 `Content-Type` 替换为 `application/json`，提交全部 pending headers，以固定 Content-Length 写出 body 并结束响应流。

#### `resp.send(status)`

- 参数：异步函数；非整数 status 回退为 200。
- 返回：发送成功为 `null`。
- 异常：header 或结束流失败时抛 `Error`。
- 副作用：提交 pending headers，发送长度为 0 的响应体并结束响应流；不自动设置 Content-Type。

#### `resp.send(status, body)`

- 参数：异步函数；非整数 status 回退为 200。
- 返回：发送成功为 `null`。
- 编码：`Binary` 按原始字节发送且不自动设置 Content-Type；字符串设置 `text/plain;charset=utf-8` 并按 UTF-8 发送；其他值按 `resp.sendJson()` 的 JSON 规则编码并设置 `application/json`。
- 异常：编码、header 或 body I/O 失败时抛 `Error`。
- 副作用：提交 pending headers，写出 body 并结束响应流。

正常脚本只应调用一次发送函数。header 成功提交后 context 会记录“响应已发送”；之后的 header 修改是无操作，再次 `send*()` 通常会成为 I/O 错误。若 body 写入在 header 提交后失败，异常仍可被 `catch`，但客户端已经收到的响应不能回滚。

### 6.3 路由和连接常量

`RouteScriptExtension` 提供下列编译期解析、请求期取值的常量：

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
    query: string;   // 不含 '?'
};

declare const $conn: {
    remote_addr: string | null;
    remote_port: number;
    http_version: "HTTP/0.9" | "HTTP/1.0" | "HTTP/1.1" | "HTTP/2" | "HTTP/3";
    scheme: string;
    tls: boolean;
};
```

示例：

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

解析规则：

- `$path.<name>` 必须与宿主创建 `RouteScriptExtension::CompileScope` 时声明的路径变量名完全一致，否则编译失败。其余四个动态 namespace 接受任意合法标识符 key；`$req`/`$conn` 只接受类型声明中列出的固定字段，未知字段也是编译错误。
- `$path`、`$query`、`$header`、`$cookie` 和 `$context` 的 package 名都会转成 ASCII 小写并把 `-` 转成 `_`，再进行匹配和去重。例如 `$header.x_forwarded_for` 匹配 `X-Forwarded-For`，`$query.foo_bar` 也可匹配 query key `Foo-Bar`。两个归一化后相同的名称共享一个槽。
- 点号后的 key 必须是脚本标识符，不能写 `$query["a.b"]`。需要精确区分大小写、连字符/下划线，或包含其他特殊字符时，应使用 `req.getQuery("...")`、`req.getHeader("...")` 或 `req.getCookie("...")`。
- 动态槽初始为 `null`：缺失的 path/query/header/cookie/context 都返回 `null`。重复 query 取最后一个匹配值；重复 header 和 cookie 取第一个匹配值；path/context 由宿主绑定。
- `$req.uri` 与 `req.getUri()` 相同；`$req.query` 不含 `?`。`$conn.remote_addr` 在地址转换失败时为 `null`，其他 `$req`/`$conn` 值直接来自当前 exchange，不进入 `ConstPackage`。
- 这些读取没有脚本副作用，也没有可捕获异常。宿主未调用 `prepare_constants()`、使用了不匹配的 package，或未传 context 时会产生 `InvalidState` abort；准备动态值时内存不足由宿主初始化流程报告，首次格式化 `$conn.remote_addr` 的请求池分配失败则产生 `OutOfMemory` abort。

编译单元中的动态名称由 `ConstPackage::Builder` 去重并分配连续 index。运行期必须先用最终的不可变 `ConstPackage` 准备请求槽位，再绑定路由和 context 值；完整 C++ 顺序见 8.2。

### 6.4 上游 HTTP 指令

上游服务必须先以指令绑定固定目标；没有独立的全局 `http.request()` 或 `http.proxyPass()`：

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

目标可以是命名上游或不带路径的 URL authority：

```javascript
directive a = http "@backend";
directive b = http "backend";
directive c = http "http://127.0.0.1:8080";
directive d = http "https://api.example.com";
```

目标字符串在脚本编译时解析：

- 以 `http://`/`https://` 开头时按固定 URL authority 处理，不能带 path、query 或 fragment；应只写 `host[:port]`。userinfo 没有专门语义，不应使用。未写端口时 HTTP 使用 80、HTTPS 使用 443。
- 方括号 IPv6 authority 当前必须显式写端口，例如 `http://[::1]:8080`。端口必须是 0..65535 的十进制整数；0 表示使用 scheme 默认端口。
- 其他非空字符串都按命名上游处理，开头的 `@` 可省略；名称是否存在会在请求期由 `HttpScriptServices` 决定。
- 每个 directive 方法只接受 0 或 1 个 options 参数，并且不接受参数展开；options 不是对象时等同于空对象。
- `options.url` 始终是请求的 `path?query`，不是主机。把完整 `http://...`/`https://...` URL 放入该字段会抛可捕获的 `Error`。

#### `service.request()`

```typescript
type RequestHeaderOverrides = {
    [name: string]: ScriptValue | null | undefined;
};

interface HttpRequestOptions {
    // 完整请求目标，优先于 path/query；这里只能是 path[?query]。
    url?: string;
    path?: string; // 默认 "/"
    query?: string | { [name: string]: ScriptValue | ScriptValue[] };
    method?: "GET" | "POST" | "PUT" | "DELETE" | "HEAD" | "OPTIONS" | "PATCH" | string;
    headers?: RequestHeaderOverrides;
    body?: ScriptValue;
    timeout?: number; // 毫秒，正整数；默认 30000
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

参数字段：

| 字段 | 规则 |
| --- | --- |
| `method` | 字符串且不区分大小写地匹配 GET、POST、PUT、DELETE、HEAD、OPTIONS、PATCH；缺省、非字符串或未知值回退为 GET |
| `url` | 非空字符串时作为完整 `path?query`，优先于 `path`/`query`；完整 HTTP(S) URL 被拒绝 |
| `path` | `url` 未生效时使用非空字符串；否则默认 `/` |
| `query` | 非空字符串原样追加（不做转义）；对象按 form-urlencoded 编码，数组属性展开为重复 key；其他类型或空结果省略 query |
| `headers` | 对象时逐项处理；`null`/`undefined` 删除字段，其他值用兼容文本设置；转换为空文本的值静默跳过 |
| `body` | 规则见下；缺省、`null`、空字符串或空 `Binary` 表示无 body |
| `timeout` | 只有正整数生效，单位毫秒；浮点数、非正数和其他类型使用 30000 |
| `includeHeaders` | 只有布尔值生效，默认 `false` |

对象 query/header 值使用 `node_json_to_string` 兼容文本：标量使用自身文本，`null` 为 `"null"`，数组/对象为节点占位文本，`Binary` 使用原始字节。query 数组的每个元素分别编码；空数组不产生字段。header 名和值是否符合 HTTP 语法最终由协议层验证。

body 编码和自动 Content-Type：

| body 类型 | 编码 | 未显式设置 Content-Type 时 |
| --- | --- | --- |
| `Binary` | 原始字节 | `application/octet-stream` |
| 字符串 | UTF-8 字节 | `text/plain;charset=utf-8` |
| 对象，且显式 Content-Type 值包含小写子串 `application/x-www-form-urlencoded` | form-urlencoded | 已有值保持不变 |
| 其他对象、数组、数字或布尔值 | JSON | `application/json;charset=utf-8` |

Content-Type 子串判断当前区分大小写。JSON/form 编码得到空 body 时按无 body 发送；当前实现的 JSON 编码失败也会退化成无 body，而不是抛异常，因此不要把迭代器、非法字符串或非有限数字作为 request body。

- 返回：成功后返回新对象 `{status, body}`；`status` 是最终非 1xx 上游状态，`body` 是完整响应字节的 `Binary`。只有 `includeHeaders === true` 时才有 `headers` 对象；完全相同的重复响应头键以后值覆盖前值。
- 异常/中止：完整 URL 写入 `options.url`，或连接池获取、DNS/连接、header/body 写入、响应读取/超时失败时抛 `Error`。缺少 services 为 `InvalidState` abort，结果分配失败为 `OutOfMemory` abort。
- 副作用：获取或建立上游连接、发送一次上游请求并完整读取响应。请求/响应 body 都连续缓冲，函数本身不设大小上限；大响应和纯转发优先使用 `proxyPass()`。

#### `service.proxyPass()`

```typescript
interface ProxyPassOptions {
    url?: string;
    path?: string;
    query?: string | { [name: string]: ScriptValue | ScriptValue[] };
    method?: string;
    headers?: RequestHeaderOverrides;
    responseHeaders?: RequestHeaderOverrides;
    timeout?: number; // 毫秒，默认 30000
    flush?: boolean;  // 默认 false；true 关闭本地响应体聚合
    websocket?: boolean;
}

interface HttpService {
    proxyPass(options?: ProxyPassOptions): number;
}
```

`proxyPass()` 把当前入站请求流式转发到已绑定上游，再把上游响应流式写回客户端。options 字段规则如下：

| 字段 | 规则 |
| --- | --- |
| `method` | 缺省/非字符串时继承入站方法；识别七种标准方法，未知字符串也回退为入站方法 |
| `url` | 非空字符串时作为完整 `path?query` 并覆盖 `path`/`query`；完整 HTTP(S) URL 被拒绝 |
| `path` | `url` 未生效时使用非空字符串；否则继承入站 path |
| `query` | 非空字符串原样替换；对象按 form-urlencoded 替换（空对象会删除入站 query）；其他类型和空字符串继承入站 query |
| `headers` | 对复制后的上游请求头执行设置/删除；值转换规则与 `request()` 相同。处理完后再次删除请求 framing headers |
| `responseHeaders` | 对过滤后的下游响应头执行设置/删除；值转换规则与 `headers` 相同 |
| `timeout` | 只有正整数生效，单位毫秒，默认 30000；用于上游获取、连接、读写和普通响应读取 |
| `flush` | 只有布尔 `true` 启用低延迟 body pipe，默认 `false` |
| `websocket` | 只有布尔 `true` 启用 WebSocket 代理，默认 `false` |

普通 HTTP 转发行为：

- 入站请求头复制到上游前会过滤 framing 和 hop-by-hop 字段；随后应用 `headers`。请求体从入站流直接写到上游，因此没有 `body` option，并且会消费当前请求体。
- 上游 1xx 响应会被跳过，直到最终响应；上游响应头过滤 hop-by-hop 字段后应用 `responseHeaders`。HEAD 和不允许 body 的状态不会向下游发送 body，剩余上游 body 会尝试丢弃。
- 已知 Content-Length 会保留固定长度 framing，否则使用自动 framing。响应体默认使用 64 KiB buffer 和 48 KiB low-water，不足 low-water 时等待更多数据或 EOF。
- `flush: true` 把 low-water 设为 0，关闭跨读取聚合；每次最多读取 64 KiB，当前块写完并 flush 后再读下一块，gzip 启用时会使用 `Z_SYNC_FLUSH`。它不自动设置 `X-Accel-Buffering: no`，也不关闭外层代理或协议栈的缓冲。普通 body pipe 的下游写入不使用 `timeout` 截止时间。

WebSocket 行为：

- `websocket: true` 要求入站请求是有效的 HTTP/1.1 WebSocket Upgrade 或 HTTP/2/3 Extended CONNECT，否则在连接上游前抛 `Error`。
- 上游方法被强制为 GET；任何非空且不能识别为 GET 的 method（包括未知方法）都抛 `Error`。函数建立上游 HTTP/1.1 Upgrade，把下游 HTTP/1.1 响应写成 101，把 Extended CONNECT 响应写成 200。
- 自定义 header 覆盖之后会重新确立握手必需字段，不能用 options 删除它们。握手成功后函数一直等待双向隧道结束，`timeout` 用作隧道单次读写超时。
- 返回值仍是上游 101，即使 Extended CONNECT 客户端看到的是 200。当前隧道 relay 的结束/错误不会转换成脚本异常，握手成功后直接返回 101。

返回、异常和副作用：

- 返回：普通模式成功返回最终上游状态码；WebSocket 模式返回上游 101。返回前，下游响应已经发送或隧道已经结束。
- 异常/中止：URL、WebSocket 握手、连接、上游/下游 header、请求体或响应体 I/O 和超时错误抛 `Error`；缺少 services 为 `InvalidState` abort。
- 副作用：消费入站请求体、发送上游请求、提交并写出下游响应。响应头提交后 context 立即标记为已发送；此后的流式错误仍可被脚本 `catch`，但响应可能已经部分到达客户端，不能改写成另一份响应。

通常应让 `service.proxyPass()` 成为最后一个有效动作，例如 `return service.proxyPass({flush: true});`。成功后不要再调用 `resp.send*()`。

## 7. C++ 嵌入 Script

### 7.1 CMake 链接

仓库内应用直接链接 `fiber_lib`；其 `include/` 目录会以 PUBLIC 方式传递，核心头文件统一通过 `<fiber/...>` 引用：

```cmake
add_executable(script_embed main.cpp)
target_link_libraries(script_embed PRIVATE fiber_lib)
```

### 7.2 编译并同步执行

```cpp
#include <cstdio>
#include <string>
#include <utility>

#include <fiber/script/JsGc.h>
#include <fiber/script/JsValue.h>
#include <fiber/script/ScriptCompiler.h>
#include <fiber/script/std/StdLibrary.h>

int main() {
    using namespace fiber::script;

    // 独立实例适合继续注册宿主函数；只使用标准库也可以用 instance()。
    std_lib::StdLibrary library;
    GcHeap heap;

    // LocalMark 让本作用域申请的临时根在离开作用域时统一释放。
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
        !gc_object_set_key(&heap, root, "count", 5, JsValue::make_integer(3))) {
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
                     compiled.error().position, compiled.error().message.c_str());
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

    // VM 返回后不再替结果值提供 GC 根。若后续读取过程可能继续在 heap
    // 上分配，先把结果放入宿主根槽。
    ValueHandle returned = heap.local_value();
    if (!returned) {
        return 1;
    }
    *returned = result.value();

    // *returned 是 JsValue，可用 js_value_type/gc_object_get_key 等 API 读取。
    return 0;
}
```

`compile_script()` 的参数：

```cpp
std::expected<Script, parse::ParseError>
compile_script(Library &library,
               std::string_view source,
               bool allow_assign = true,
               std::size_t max_depth = 128);
```

- `library` 决定编译时可见的函数、常量和 directive。
- `allow_assign=false` 禁止表达式中的赋值，适合条件和只读配置表达式。
- `max_depth` 同时约束解析和编译嵌套深度；0 会按 1 处理。
- `ParseError::position` 是源文本位置，`message` 是简短错误说明。

### 7.3 执行结果

```typescript
type ScriptExecutionResult =
    | { kind: "Value"; value: ScriptValue }
    | { kind: "Void" }
    | { kind: "Exception"; exception: ScriptValue }
    | { kind: "Abort"; reason: ScriptAbortReason; position: number };
```

C++ 对应 `ScriptResultKind`：

- `Value`：脚本执行了 `return expression;`。即使 expression 的值为 `undefined`，仍是 `Value`。
- `Void`：脚本执行 `return;` 或运行到文件末尾。`Value` 和 `Void` 都满足 `is_success()`。
- `Exception`：存在未捕获的脚本异常，通过 `exception()` 读取。
- `Abort`：运行时中止，通过 `abort().reason` 和 `abort().position` 读取。

不要仅用 `has_value()` 判断执行是否正常，因为 `Void` 是正常结果但不携带值。推荐先按 `result.kind` 分支。

### 7.4 异步执行

```cpp
fiber::async::Task<fiber::script::ScriptResult>
run(fiber::script::Script &script,
    fiber::script::JsValue root,
    void *attach,
    fiber::script::GcHeap &heap) {
    co_return co_await script.exec_async(root, attach, heap);
}
```

- `exec_async()` 既能执行同步脚本，也能执行异步脚本。
- `exec_sync()` 遇到异步 opcode 会触发 `FIBER_PANIC`，所以必须先检查 `contains_async()`。
- `root`、`attach`、`heap` 及 `attach` 指向的数据必须活到返回的 coroutine 完成。
- 不要在同一个 `GcHeap` 上并发执行多个脚本；通常每个请求创建一个堆，或由明确串行的执行上下文独占一个堆。

### 7.5 GC 根和生命周期

所有堆对象都属于创建它们的 `GcHeap`。以下规则非常重要：

- 使用 `ValueHandle` 而不是裸 `JsValue` 保存“后续分配期间仍必须存活”的临时堆值。
- `heap.local_value()` 分配临时根槽，配合 `GcHeap::LocalMark` 批量回收；`heap.global_value()` 分配持续到堆析构的根槽。
- VM 执行期间会自动把 root、变量、栈、常量缓存和异步参数注册为 GC 根。
- VM 返回后，`ScriptResult` 中的值不再自动注册为根；如果消费结果时还会在同一堆上分配，应立即把结果复制到 `local_value()`/`global_value()` 根槽。
- `JsValue::make_native_string()`/`make_native_binary()` 创建借用值，宿主必须保证底层内存的生命周期覆盖所有使用。
- `JsValue::make_string()`/`make_binary()` 把内容复制进 GC 堆。
- 不要在堆析构后保存或读取指向其对象的 `JsValue`。
- 某些编译期字符串结果会借用 `Script` 的编译数据，因此在读取执行结果期间应保留对应 `Script`。最简单的规则是：`Script` 和 `GcHeap` 都至少活到结果消费完毕。

### 7.6 注册同步宿主函数

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
                JsValue::make_exception(fiber::script::ExceptionKind::TypeError));
    }

    return AbiResult::success(JsValue::make_integer(
            fiber::script::js_value_int64(args.args[0]) +
            fiber::script::js_value_int64(args.args[1])));
}

} // namespace

void register_host_functions(fiber::script::std_lib::StdLibrary &library) {
    using Signature = fiber::script::Library::FunctionSignature;
    library.register_func(
            "host.add",
            Signature{.required_argc = 2, .fixed_argc = 2, .variadic = false},
            &add,
            nullptr,
            "host.add");
}
```

脚本可直接调用：

```javascript
return host.add(20, 22);
```

宿主 ABI：

```cpp
struct HostCallFrame {
    GcHeap &runtime; // 本次执行的堆
    JsValue root;    // 本次执行的根值
    void *attach;    // exec_*() 传入的宿主上下文
};

struct Arguments {
    ConstValueHandle args;
    std::uint32_t argc;
};
```

参数已经完成重载匹配、默认参数补齐和 spread 展开。宿主函数仍需检查类型和运行时状态，并且必须 `noexcept`。

`FunctionSignature` 规则：

- 固定签名匹配 `required_argc <= argc <= fixed_argc`。
- `variadic=true` 时匹配 `argc >= fixed_argc`，可变参数从固定参数之后开始。
- 默认值只能覆盖尾部固定参数，数量必须等于 `fixed_argc - required_argc`。
- 当前同一签名不能同时使用默认值和 variadic tail。

带默认值的注册示例：

```cpp
library.register_func(
        "host.read",
        Signature{.required_argc = 1, .fixed_argc = 2, .variadic = false},
        {fiber::script::JsValue::make_integer(1000)}, // timeout 默认值
        &host_read,
        service_ptr,
        "host.read");
```

`userdata` 是非拥有指针。它以及函数返回的扩展对象必须比所有引用它们的已编译 `Script` 活得更久。

### 7.7 注册异步函数和常量

异步函数返回 `fiber::script::AsyncTask`：

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
        Signature{.required_argc = 1, .fixed_argc = 1, .variadic = false},
        &lookup_async,
        &service,
        "host.lookup");
```

如果异步函数在挂起后仍使用参数，`HostCallFrame` 和 `Arguments` 视图在该次异步宿主调用期间保持有效；不要把这些视图保存到 `AsyncTask` 完成之后。

同步常量的注册 key 使用 `$namespace/key`，脚本通过 `$namespace.key` 访问：

```cpp
fiber::script::AbiResult environment_name(
        void *userdata,
        const fiber::script::Library::HostCallFrame &frame) noexcept {
    auto name = *static_cast<const std::string_view *>(userdata);
    auto value = fiber::script::JsValue::make_string(
            frame.runtime, name.data(), name.size());
    if (fiber::script::js_value_type(value) != fiber::script::JsNodeType::String) {
        return fiber::script::AbiResult::abort(
                fiber::script::ScriptAbortReason::OutOfMemory);
    }
    return fiber::script::AbiResult::success(value);
}

library.register_constant(
        "$env/name", &environment_name, &environment, "$env.name");
```

对应脚本：

```javascript
return {environment: $env.name};
```

更复杂、需要按名称动态解析函数/常量/directive 的宿主可以实现 `StdLibrary::ExtOps` 并通过 `add_ext_ops()` 安装。扩展按注册顺序作为 fallback 查询，标准库中已注册的同名项优先。

## 8. C++ 嵌入 HTTP Script

### 8.1 初始化和编译

```cpp
#include <expected>
#include <memory>
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
        library.add_ext_ops(&exchange_constants,
                            fiber::http_script::ExchangeConstExtension::ops());
        library.add_ext_ops(&route_extension,
                            fiber::http_script::RouteScriptExtension::ops());
    }
};

std::expected<fiber::script::Script, fiber::script::parse::ParseError>
compile_route_script(ScriptRuntime &runtime,
                     fiber::http_script::ConstPackage::Builder &constants,
                     std::string_view source,
                     const std::vector<std::string> &path_variables) {
    // 同一配置快照中的所有脚本共享 builder；CompileScope 只在本次串行编译期间有效。
    fiber::http_script::RouteScriptExtension::CompileScope scope(
            runtime.route_extension, constants, path_variables, true);
    return fiber::script::compile_script(runtime.library, source);
}

// 所有脚本编译成功后调用一次，并把 package 与这些脚本放进同一个不可变快照。
// auto package = constants.build();
```

生命周期要求：

- `ConstPackage::Builder` 只在编译期可变；`build()` 后不可再添加常量。
- `ConstPackage` 拥有动态常量 HostCallable 的 userdata，必须与使用它编译出的脚本一起存入快照并至少同寿命。
- `build()` 生成按 type 分区的紧凑 Entry/桶数组；每个分区使用不超过 50% 装载率的二次探测哈希。编译期的去重表、顺序表和 `HostCallable` 不进入不可变 package，package 只保留脚本 userdata 所需的稳定引用与归一化名称。
- `ExchangeConstExtension` 的固定 userdata 使用静态存储；`$req`/`$conn` 执行时直接构造 native value，不依赖 package identity。
- `StdLibrary` 只参与编译；含 HTTP directive 的脚本仍要求 `RouteScriptExtension` 持有的 directive 定义比脚本活得更久，因为编译结果中的上游函数 userdata 指向这些定义。
- `CompileScope` 修改扩展的临时编译上下文，因此共享扩展时必须串行编译；脚本执行不读取这份可变状态。
- 只编译同步模板时应把 HTTP directives 关闭，并在编译后拒绝 `contains_async()`。
- 运行期绑定进常量槽的借用文本，以及 `$req` 借用的 exchange 文本和 `ScriptConnectionInfo::scheme`，都必须持续到该次脚本执行结束；query 解码结果由 `prepare_constants()` 自动复制进请求池，`$conn.remote_addr` 首次访问时格式化并缓存在请求池。

### 8.2 每请求执行

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
        const std::vector<std::pair<std::string_view, std::string_view>> &path_vars,
        fiber::http_script::HttpScriptServices *services,
        fiber::http_script::ScriptConnectionInfo connection) {
    // 也可以传 exchange.pool() 构造堆，让 GC 对象使用请求的 BufPool。
    fiber::script::GcHeap heap(exchange.pool());
    fiber::http_script::ScriptExchangeCtx context(exchange, heap, connection);
    auto prepared = context.prepare_constants(constants);
    if (!prepared || !context.bind_path_constants(constants, path_vars)) {
        (void) co_await context.write_error_json(500, "SCRIPT_CONSTANTS");
        co_return;
    }
    context.set_services(services);

    auto result = co_await script.exec_async(
            fiber::script::JsValue::make_undefined(),
            &context,
            heap);

    // 若脚本调用了 resp.send*() 或 svc.proxyPass()，响应已经发送。
    if (context.response_header_sent()) {
        co_return;
    }

    // 在 heap/context 仍存活时消费 ScriptResult，并生成兜底响应。
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
            } else if (js_value_type(exception) == JsNodeType::Exception) {
                (void) co_await context.write_error_json(
                        500,
                        fiber::script::exception_kind_name(
                                js_value_exception_kind(exception)));
            } else {
                // 脚本也允许 throw 任意值；这里选择统一隐藏其内容。
                (void) co_await context.write_error_json(500, "ScriptException");
            }
            break;
        }
        case ScriptResultKind::Abort:
            (void) co_await context.write_error_json(
                    500,
                    fiber::script::abort_reason_name(result.abort().reason));
            break;
    }
    co_return;
}
```

必须像上例一样在局部 `heap` 和 `context` 仍然存活时消费结果；返回后不能再解引用其中的堆值。`apps/lite_nginx/src/runtime/ServerLauncher.cpp` 的 `run_script()` 是完整参考实现。

`$context` 等宿主值也按编译期确定的 index 设置。例如：

```cpp
auto index = constants.find(fiber::http_script::ConstType::Context, "cluster");
if (index != fiber::http_script::kInvalidConstIndex) {
    context.bind_constant(index, cluster);
}
```

也可以把一组 `IndexedConstValue` 传给 `prepare_constants()` 或 `bind_constants()`。未设置的动态槽保持 `null`；动态常量 HostCallable 只校验 package identity 和 index，然后直接返回槽值，不再按名称扫描请求数据。`$req`/`$conn` 由 `ExchangeConstExtension` 直接读取，不占用这些槽。

### 8.3 提供上游连接服务

`svc.request()` 和 `svc.proxyPass()` 依赖宿主实现 `HttpScriptServices`：

```cpp
class HttpScriptServices {
public:
    virtual ~HttpScriptServices() = default;

    virtual fiber::async::Task<fiber::common::IoResult<
            std::unique_ptr<fiber::http_script::HttpUpstreamConnection>>>
    acquire(const fiber::http_script::HttpTargetSpec &target,
            std::chrono::milliseconds connect_timeout) noexcept = 0;
};
```

`acquire()` 必须返回已经连接的 HTTP/1 上游连接 holder。holder 的生命周期代表连接租约；销毁时应把可复用连接归还连接池，或释放临时连接。命名上游的选择、DNS、TLS/SNI、连接池 key 和连接建立策略由宿主负责。

lite-nginx 已提供基于 `UpstreamRegistry + ConnectionPool + DnsService` 的实现，位于 `apps/lite_nginx/src/runtime/HttpScriptServices.*`，自定义应用可复用其设计。

如果脚本不使用上游 directive，`services` 可以为 null；一旦调用 `svc.request()`/`svc.proxyPass()`，缺少服务会产生 `InvalidState` abort。

## 9. 常见问题和约束

### 为什么脚本里没有 `await`？

异步性是宿主函数注册信息的一部分。编译器为异步调用生成异步 opcode，VM 在该位置自动挂起并恢复。脚本源码保持同步写法，但 C++ 必须使用 `exec_async()`。

### 为什么调用函数时就编译失败？

函数名和参数数量都在编译期解析。确认使用了正确的 `Library` 实例，并且在编译前完成 `register_*`/`add_ext_ops()`。HTTP 函数还需要先调用 `register_http_functions_to_lib()`。

### 为什么 `req.*` 返回 `InvalidState`？

执行时没有把当前 `ScriptExchangeCtx*` 作为 `attach` 传给 `exec_*()`，或者上下文已失效。`req.*`/`resp.*` 不从全局状态获取当前请求。

### 为什么 `exec_sync()` 直接 panic？

脚本包含异步 opcode。编译后检查 `contains_async()`，请求脚本统一使用 `exec_async()`；只读配置模板则应拒绝异步脚本。

### 为什么响应没有发送或发送了两次？

执行结束后先检查 `ScriptExchangeCtx::response_header_sent()`。若为 `true`，脚本已调用 `resp.send*()` 或 `svc.proxyPass()`；宿主不应再发送兜底响应。若为 `false`，宿主必须根据 `ScriptResult` 决定响应。

### 为什么 `$path.name` 编译失败？

当前路由编译上下文没有声明该变量。先从路由模式提取变量名，用这些名称创建 `RouteScriptExtension::CompileScope`，再编译脚本。这个检查有意放在编译期。

### 为什么上游 `url` 被拒绝？

上游主机必须绑定在 `directive service = http "...";` 中。`service.request({url: ...})` 和 `service.proxyPass({url: ...})` 的 `url` 只能是请求的 `path?query`。

### 线程和并发应如何处理？

- 不要在脚本编译期间并发修改同一个 `StdLibrary` 或扩展上下文。
- 不要并发使用同一个 `GcHeap`/`ScriptExchangeCtx`。
- 已编译 `Script` 的字节码是只读的；是否跨线程共享还需要宿主保证其中所有函数 userdata 和扩展状态满足相应线程安全要求。
- 每请求一个 `GcHeap` 和 `ScriptExchangeCtx` 是最简单的所有权模型。

## 10. 相关源码和示例

- 编译入口：`include/fiber/script/ScriptCompiler.h`
- 执行入口：`include/fiber/script/Script.h`、`include/fiber/script/ScriptResult.h`
- C++ 值/GC API：`include/fiber/script/JsValue.h`、`include/fiber/script/JsGc.h`
- 标准库注册：`src/script/std/StdLibrary.cpp`
- HTTP 函数与固定常量：`src/http_script/RequestFuncs.cpp`、`ResponseFuncs.cpp`、`ExchangeConstExtension.cpp`
- 上游 HTTP：`src/http_script/HttpClientFuncs.cpp`
- HTTP 执行上下文：`include/fiber/http_script/ScriptExchangeCtx.h`
- lite-nginx 脚本示例：`apps/lite_nginx/conf/scripts/`
- 完整 HTTP 执行参考：`apps/lite_nginx/src/runtime/ServerLauncher.cpp`
- 标准库行为测试：`tests/*FuncsTest.cpp`
- HTTP API 行为测试：`tests/HttpScriptFuncsTest.cpp`
