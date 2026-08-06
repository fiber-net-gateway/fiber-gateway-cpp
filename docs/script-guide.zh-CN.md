# Script 模块使用指南

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

## 4. 支持的脚本语法

### 4.1 字面量、变量和访问

```javascript
let count = 3;
let ratio = 0.25;
let enabled = true;
let empty = null;
let text = "fiber";
let list = [1, 2, count];
let object = {name: text, enabled, ["dynamic-key"]: 1};

let first = list[0];
let name = object.name;
object.name = "gateway";
object["new-key"] = 2;
```

对象字面量支持简写属性、计算属性和展开，数组支持展开：

```javascript
let name = "fiber";
let base = {a: 1};
let object = {name, ...base, ["b"]: 2};
let list = [0, ...[1, 2], 3];
```

`let` 声明必须以分号结束，其他语句也建议始终写分号。对象字面量中重复的静态键是编译错误。数组下标赋值只能修改已有下标；追加元素应使用 `array.push()`。

C++ 传给 `exec_sync()`/`exec_async()` 的 `root` 参数在脚本内以 `$` 访问，例如 `$.user.id`。`$query.name` 这类写法则是由宿主库解析的常量，不是根对象属性。

### 4.2 运算符

支持以下运算符：

```text
一元：+  -  !  typeof
算术：+  -  *  /  %
比较：<  <=  >  >=  ==  !=  ===  !==
逻辑：&&  ||
成员：in
条件：condition ? whenTrue : whenFalse
赋值：=
```

`&&` 和 `||` 短路执行并返回参与运算的值。`+` 在任一操作数是字符串时执行字符串拼接；对象、数组和二进制不能隐式拼接。数值运算接受数字、布尔值和 `null`，不支持普通 JavaScript 那样把任意字符串隐式转成数字。

`==`/`!=` 是宽松比较，`===`/`!==` 是严格比较。数组和对象的严格比较使用引用身份。

`key in object` 检查对象属性是否存在；`index in array` 检查整数下标是否落在数组范围内。其他类型组合返回 `false`。

`~` match 运算符虽然可被语法解析，但当前会在字节码编译阶段被拒绝；正则相关能力尚未提供。

### 4.3 控制流

```javascript
let result = [];

if ($.enabled) {
    for (let key, value of $.items) {
        if (value == null) {
            continue;
        }
        array.push(result, value);
        if (length(result) >= 10) {
            break;
        }
    }
} else {
    return [];
}

return result;
```

`for` 只支持以下双变量 `for-of` 形式：

```typescript
// 数组：key 为从 0 开始的下标，value 为元素。
for (let key, value of array) { /* ... */ }

// 对象：key 为属性名，value 为属性值，按插入顺序迭代。
for (let key, value of object) { /* ... */ }
```

迭代期间新增/删除数组元素或对象属性属于结构修改，会产生可捕获的 `IterationError`；更新已有对象属性的值不属于结构修改。

当前不支持 `while`、传统三段式 `for`、函数/类声明、模块、Promise 或 `await`。

### 4.4 异常

```javascript
try {
    let value = JSON.parse($.input);
    return value;
} catch (error) {
    return {ok: false, error};
}
```

脚本可用 `throw value;` 抛出任意值。标准库的类型、范围和解析错误也可以被 `try/catch` 捕获。

C++ 结果中的 `Exception` 和 `Abort` 必须区别处理：

- `Exception` 是脚本语义错误，可被脚本内 `try/catch` 捕获；未捕获时成为 `ScriptResultKind::Exception`。
- `Abort` 是运行时终止，例如 `OutOfMemory`、`InvalidState`、`Timeout`，不能被脚本捕获。

### 4.5 模板字符串

脚本内支持反引号模板字符串：

```javascript
let id = 42;
return `item-${id}-${1 + 2}`;
```

C++ 还提供 `compile_template_string()`，输入是“不带外层反引号”的模板正文：

```text
prefix-${$.name}-${length($.items)}
```

模板始终返回字符串。`compile_template_string()` 默认禁止赋值，但不会自动禁止异步函数；准备通过 `exec_sync()` 执行模板时，宿主必须检查 `Script::contains_async()`。

### 4.6 函数解析和参数数量

所有函数都在编译期解析，调用未知函数、参数数量不匹配或重载歧义都会导致编译失败。可变参数和展开参数受签名约束：

```javascript
array.push(items, 1, 2);
array.push(items, ...moreItems);
Object.assign(target, ...sources);
```

函数调用是静态名称查找。`array.push(a, x)` 是标准库函数；`a.push(x)` 不是动态对象方法，除非宿主显式注册了这个完整函数名。

## 5. 标准库 API

`fiber::script::std_lib::StdLibrary` 构造时会注册本节全部函数。除特别说明外，参数数量错误在编译阶段报错；下文所说的 `TypeError`、`RangeError`、`SyntaxError` 是执行阶段可捕获异常。

当前完整函数名如下，便于按脚本中的实际调用名检索：

```text
length                         includes
array.join                     array.pop                       array.push
strings.hasPrefix              strings.hasSuffix               strings.toLower
strings.toUpper                strings.trim                    strings.trimLeft
strings.trimRight              strings.split                   strings.contains
strings.contains_any           strings.index                   strings.indexAny
strings.lastIndex              strings.lastIndexAny            strings.repeat
strings.substring              strings.toString
binary.base64Encode            binary.base64Decode             binary.hex
binary.fromHex                 binary.getUtf8Bytes
hash.crc32                     hash.md5                         hash.sha1
hash.sha256
math.floor                     math.abs
rand.random                    rand.canary
JSON.parse                     JSON.stringify
Object.assign                  Object.keys                     Object.values
Object.deleteProperties
URL.encodeComponent            URL.decodeComponent             URL.parseQuery
URL.buildQuery
```

### 5.1 通用函数

```typescript
function length(value?: ScriptValue): number;
function includes(container: string | ScriptValue[], ...items: ScriptValue[]): boolean;
```

`length(value)`：

- 省略参数等同于传入 `null`。
- 字符串返回 UTF-16 code unit 数；`"😀"` 的长度是 2。
- `Binary` 返回字节数，数组返回元素数，对象返回属性数。
- `null`、`undefined`、数字、布尔值及其他类型返回 0。

`includes(container, ...items)`：

- 字符串容器要求每个 `item` 都是其子串。
- 数组容器要求每个 `item` 都能以严格相等 `===` 找到。
- 其他容器返回 `false`。
- 没有 `items` 时，合法的字符串或数组容器返回 `true`。

### 5.2 数组函数

```typescript
namespace array {
    function join(values: ScriptValue[], separator?: ScriptValue): string;
    function pop(values: ScriptValue[]): ScriptValue | null;
    function push(values: ScriptValue[], ...items: ScriptValue[]): ScriptValue[];
}
```

- `array.join()` 的默认分隔符是空字符串，不是 JavaScript `Array.prototype.join()` 的逗号。字符串、数字和布尔值会转成文本；`null`、`undefined`、容器和二进制按空文本处理。非数组抛出 `TypeError`。
- `array.pop()` 删除并返回最后一个元素；空数组返回 `null`。非数组抛出 `TypeError`。
- `array.push()` 原地追加所有 `items`，返回数组自身而不是新长度。非数组抛出 `TypeError`。

### 5.3 字符串函数

```typescript
namespace strings {
    function hasPrefix(text: string, prefix: string): boolean;
    function hasSuffix(text: string, suffix: string): boolean;
    function toLower(text: string): string | null;
    function toUpper(text: string): string | null;

    function trim(text: string, cutset?: string | null): string | null;
    function trimLeft(text: string, cutset?: string | null): string | null;
    function trimRight(text: string, cutset?: string | null): string | null;
    function split(text: string, separators?: string | null): string[] | null;

    function contains(text: string, value: string): boolean | null;
    function contains_any(text: string, chars: string): boolean | null;
    function index(text: string, value: string): number | null;
    function indexAny(text: string, chars: string): number | null;
    function lastIndex(text: string, value: string): number | null;
    function lastIndexAny(text: string, chars: string): number | null;

    function repeat(text: string, count: number): string | null;
    function substring(text: string, start?: number, end?: number): string | null;

    function toString(): string;
    function toString(value: ScriptValue): string;
}
```

共同规则：需要字符串的参数若类型错误，多数函数按兼容语义返回 `null`，`hasPrefix()`/`hasSuffix()` 返回 `false`，而不是抛异常。

- `toLower()`/`toUpper()` 当前只转换 ASCII `A-Z`/`a-z`，非 ASCII 字符保持原字节不变。
- `trim(text)` 删除两端值不大于 `0x20` 的字符。传入 `cutset` 时，`cutset` 被当作一个完整子串，从对应端反复删除，不是字符集合。
- `trimLeft()`/`trimRight()` 未传 `cutset` 时删除当前实现支持的 ASCII Java whitespace；传入 `cutset` 时同样反复删除完整子串。
- `split(text)` 未传分隔符时返回 `[text]`。传入 `separators` 后，它被当作 Unicode code point 集合，其中任一字符都可分隔；连续和尾部分隔符不产生空元素。
- `contains_any()`/`indexAny()` 把 `chars` 当作 code point 集合。
- `index()`、`indexAny()`、`lastIndex()` 返回 UTF-16 下标，找不到返回 `-1`。
- `lastIndexAny()` 为兼容既有语义，会按 `chars` 中的字符顺序查找，返回“第一个在 `text` 中出现的候选字符”的最后位置，不是所有候选字符位置的全局最大值。
- `repeat()` 接受整数或浮点数，浮点数向零截断；负数或非数值返回 `null`。单次结果限制为 16 MiB，超过限制会以 `OutOfMemory` 中止。
- `substring()` 的 `start`/`end` 是 UTF-16 下标，默认分别为 0 和 `2147483647`；负的 `start` 会归零，`end <= start` 返回空字符串。
- `toString()` 无参数返回空字符串；`null`/`undefined` 返回 `"null"`。对象、数组当前分别返回 `"<ObjectNode>"`、`"<ArrayNode>"`，它不是 JSON 序列化；需要 JSON 时使用 `JSON.stringify()`。

正则函数 `strings.match` 和 `strings.findAll` 当前没有注册。

### 5.4 二进制函数

```typescript
namespace binary {
    function base64Encode(value: Binary): string | undefined;
    function base64Decode(value: string): Binary | undefined;
    function hex(value: Binary): string;
    function fromHex(value: string): Binary;
    function getUtf8Bytes(value: ScriptValue): Binary;
}
```

- `base64Encode()` 只接受 `Binary`，其他类型返回 `undefined`。
- `base64Decode()` 只接受字符串，其他类型返回 `undefined`；非法字符、非法填充、非 4 倍数长度或空白字符会抛 `RangeError`。
- `hex()` 返回小写十六进制，非二进制抛 `TypeError`。
- `fromHex()` 严格要求偶数长度和合法十六进制字符，非法输入抛 `RangeError`，非字符串抛 `TypeError`。
- `getUtf8Bytes()` 把当前兼容文本表示编码为 UTF-8 字节。标量按文本编码；对象和数组分别编码字面文本 `<ObjectNode>`、`<ArrayNode>`。它不是 `JSON.stringify()` 的二进制版本。

### 5.5 哈希函数

```typescript
namespace hash {
    function crc32(value: ScriptValue): number;
    function md5(value: string | Binary): string;
    function sha1(value: string | Binary): string;
    function sha256(value: string | Binary): string;
}
```

- `crc32()` 对值的兼容文本表示计算 CRC-32，返回 0 到 `0xffffffff` 范围内的整数。容器和二进制的兼容文本为空。
- `md5()`、`sha1()`、`sha256()` 接受 UTF-8 字符串或原始二进制，返回小写十六进制摘要；其他类型抛 `TypeError`。

MD5/SHA-1 仅用于互操作，不应用于需要抗碰撞性的安全设计。

### 5.6 数学和随机函数

```typescript
namespace math {
    function floor(value: number): number;
    function abs(value: number): number;
}

namespace rand {
    function random(max?: number): number;
    function canary(ratio: number, ...keys: ScriptValue[]): boolean;
}
```

- `math.floor()`：整数原样返回，浮点数向负无穷取整并返回整数；非数字抛 `TypeError`。
- `math.abs()`：返回绝对值；为兼容 Java，`INT64_MIN` 保持原值；非数字抛 `TypeError`。
- `rand.random(max)`：返回 `[0, max)` 内均匀整数，`max` 默认 1000。浮点上限向零截断，`max <= 0` 抛 `RangeError`，非数字抛 `TypeError`。
- `rand.canary(ratio, ...keys)`：`ratio <= 0` 恒为 `false`，`ratio >= 100` 恒为 `true`。没有 key 时随机分桶；有 key 时按参数顺序计算累计 CRC-32，结果稳定地映射到 `[0, 100)`。非数值 ratio 按 0 处理。

### 5.7 JSON 函数

```typescript
namespace JSON {
    function parse(text: string): ScriptValue;
    function stringify(value: ScriptValue): string | undefined;
}
```

- `JSON.parse()` 只接受字符串。非法 JSON 抛带解析消息和字节偏移的 `SyntaxError`，非字符串抛 `TypeError`。
- `JSON.stringify(undefined)` 返回 `undefined`；嵌套的 `undefined` 编码为 JSON `null`。
- 顶层 `NaN` 和正负无穷编码为字符串 `"null"`；其他不合法数值或无法编码的值抛 `TypeError`。
- `Binary` 编码为 Base64 JSON 字符串。
- 对象属性按插入顺序输出。

### 5.8 对象函数

```typescript
namespace Object {
    function assign(
        target: ScriptObject,
        source: ScriptValue,
        ...sources: ScriptValue[]
    ): ScriptObject;

    function keys(value: ScriptObject): string[];
    function values(value: ScriptObject): ScriptValue[];

    function deleteProperties(
        target: ScriptObject,
        key: ScriptValue,
        ...keys: ScriptValue[]
    ): ScriptObject;
}
```

- `Object.assign()` 原地合并对象来源并返回 `target`。非对象来源静默跳过；非对象 `target` 抛 `TypeError`。覆盖属性不会改变其插入位置。
- `Object.keys()`/`Object.values()` 按属性插入顺序返回新数组，非对象抛 `TypeError`。
- `Object.deleteProperties()` 原地删除所有字符串 key 并返回 `target`。非字符串 key 和不存在的 key 静默跳过；非对象 `target` 抛 `TypeError`。

### 5.9 URL 表单函数

这些函数实现 `application/x-www-form-urlencoded` 语义，不是 ECMAScript 全局 `encodeURIComponent`：空格编码为 `+`，`+` 解码为空格。

```typescript
namespace URL {
    function encodeComponent(value: string): string;
    function decodeComponent(value: string): string;

    function parseQuery(value: string): {
        [key: string]: string | string[];
    };

    function buildQuery(
        value?: null | undefined | { [key: string]: ScriptValue | ScriptValue[] }
    ): string | null | undefined;
}
```

- `encodeComponent()` 保留字母、数字、`-`、`_`、`.`、`*`，空格变成 `+`，其余 UTF-8 字节用大写 `%HH` 编码。非字符串抛 `TypeError`。
- `decodeComponent()` 解码 `+` 和 `%HH`，非法百分号转义抛 `RangeError`，非字符串抛 `TypeError`。
- `parseQuery()` 返回对象。重复 key 会先由字符串提升为数组并继续追加；没有 `=` 的字段值为空字符串；空段被跳过。非法转义抛 `RangeError`。
- `buildQuery()` 按对象属性插入顺序生成查询串。数组值展开为重复 key，空数组不产生字段；`null`/`undefined` 参数原样返回，其他非对象参数抛 `TypeError`。

## 6. HTTP Script API

HTTP API 不是 `StdLibrary` 的默认内容。C++ 宿主必须调用 `register_http_functions_to_lib()`，并在执行时把 `ScriptExchangeCtx*` 作为 `attach` 参数传入。

HTTP 函数的完整注册名为：

```text
req.getHeader      req.getQuery       req.getCookie       req.getUri
req.getPath        req.getQueryStr    req.getMethod       req.readJson
req.readBinary     req.discardBody
resp.setHeader     resp.addHeader     resp.addCookie      resp.sendJson
resp.send
```

### 6.1 请求函数 `req.*`

```typescript
declare namespace req {
    function getHeader(): { [name: string]: string };
    function getHeader(name: string): string | undefined | null;

    function getQuery(): { [name: string]: string };
    function getQuery(name: string): string | undefined;

    function getCookie(): { [name: string]: string };
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

#### 请求元数据

- `getHeader()` 返回延迟构造并在本次执行内缓存的请求头对象。同名字段覆盖为最后一次写入的值。
- `getHeader(name)` 按 HTTP 头名称规则查找。不存在返回 `undefined`；空名称或非字符串参数返回 `null`。
- `getQuery()` 解析原始查询串并返回对象，重复 key 保留最后一个值。当前请求侧解析对错误百分号转义是宽松的：保留已经解析出的部分。
- `getQuery(name)` 和 `getCookie(name)` 在不存在、空名称或非字符串参数时返回 `undefined`。
- `getCookie()` 解析所有 `Cookie` 头并返回对象，同名 cookie 后值覆盖前值。
- `getUri()` 返回原始请求目标 `path[?query]`，不含 scheme 和 host。
- `getPath()` 返回解析后的 path；`getQueryStr()` 返回不含 `?` 的原始 query；`getMethod()` 返回方法名。

#### 请求体

- `readJson()` 读取完整请求体并解析 JSON。空请求体、读取失败或非法 JSON 都抛出可捕获的 `Error`。
- `readBinary()` 读取完整请求体并返回原始字节；空请求体返回长度为 0 的 `Binary`。
- `discardBody()` 排空请求体并返回 `null`，底层排空错误当前被忽略。
- 请求体是流式资源。`readJson()`、`readBinary()`、`discardBody()` 和 `svc.proxyPass()` 都会消费它，不应在同一请求中组合多次读取。
- `readJson()`/`readBinary()` 会把完整 body 连续化到内存，函数自身不设置大小上限；公网服务应在 HTTP 层配置请求体限制。

### 6.2 响应函数 `resp.*`

```typescript
type HeaderValue = string | number | boolean | null;

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
    function setHeader(name: string, value: HeaderValue): null;
    function addHeader(name: string, value: HeaderValue): null;
    function addCookie(cookie: ResponseCookie): boolean;

    // 异步宿主函数。
    function sendJson(status: number, body: ScriptValue): null;
    function send(status: number): null;
    function send(status: number, body: ScriptValue): null;
}
```

- `setHeader()` 替换同名待发送响应头；`addHeader()` 追加一个同名响应头。名称或文本化后的值为空时抛 `Error`。响应头已经发出后，两者静默不再修改响应。
- `addCookie()` 把对象编码成 `Set-Cookie` 并追加到响应头。`name` 必填，`maxAge` 只接受整数，布尔字段只接受布尔值，`sameSite` 大小写敏感。成功返回 `true`，对象或字段无效返回 `false`。
- `sendJson()` JSON 编码 `body`，设置 `Content-Type: application/json`，发送固定长度响应。`status` 不是整数时回退为 200。
- `sendJson()` 中的 `undefined` 编码为 JSON `null`，`Binary` 编码为 Base64 JSON 字符串。
- `send(status)` 发送空响应体。
- `send(status, body)` 根据 body 类型选择编码：`Binary` 原样发送；字符串以 UTF-8 发送并设置 `text/plain;charset=utf-8`；其他值走 JSON 编码并设置 `application/json`。
- 发送函数会提交响应头和结束响应流。正常脚本只应调用一次 `send*()`；发送后再修改头或再次发送没有可靠语义。

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

- `$path.<name>` 必须是宿主创建 `RouteScriptExtension::CompileScope` 时声明的路径变量，否则脚本编译失败。运行时找不到时返回 `null`。
- `$req` 只允许 `uri`、`method`、`path`、`query`，`$conn` 只允许上面列出的五个字段；未知字段是编译错误。
- `$query.<key>` 和 `$context.<key>` 可按任意合法标识符编译，不存在返回 `null`。
- `$header`/`$cookie` 的 key 在匹配时转成 ASCII 小写并把 `-` 折叠为 `_`，所以 `$header.x_forwarded_for` 可以读取 `X-Forwarded-For`。
- 点号后的 key 必须是脚本标识符。含其他特殊字符的 query/cookie key 应使用 `req.getQuery("...")` 或 `req.getCookie("...")`。
- 编译单元中的常量名由 `ConstPackage::Builder` 归一化、去重并分配连续 index；运行期先通过不可变 `ConstPackage` 准备槽位，再按 index 绑定 `$path`/`$context` 等宿主值。

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

`http(s)://` 目标不能包含 path、query、fragment 或 userinfo。未写端口时 HTTP 使用 80、HTTPS 使用 443；当前方括号 IPv6 authority 必须显式携带端口，例如 `http://[::1]:8080`。`options.url` 始终是请求的 `path?query`，不是上游主机；把完整 `http://...` URL 放入 `options.url` 会抛出明确的运行时 `Error`。

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

行为说明：

- 默认方法为 GET，默认请求目标为 `/`。未知 method 当前回退为 GET。
- `url` 非空时完全覆盖 `path` 和 `query`。否则 query 字符串直接追加，对象按 form-urlencoded 编码，数组属性展开为重复 key。
- `headers` 中 `null`/`undefined` 删除字段，其他值按兼容文本表示设置字段。
- `Binary` body 原样发送，默认 `application/octet-stream`；字符串默认 `text/plain;charset=utf-8`；其他值默认 JSON 和 `application/json;charset=utf-8`。
- 当显式 `Content-Type` 包含 `application/x-www-form-urlencoded` 且 body 是对象时，body 按表单编码。
- 空字符串、空二进制、`null`、`undefined` 作为无 body 发送。
- 返回体总是完整缓冲成 `Binary`。只有 `includeHeaders: true` 时才返回 `headers`；重复响应头当前在对象中折叠为一个值。
- `request()` 自身不设置上游响应体大小上限；大响应或纯转发场景优先使用流式的 `proxyPass()`，并由宿主设置协议层限制。
- 连接、发送、读取或超时错误抛出可捕获的 `Error`。

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

`proxyPass()` 把当前入站请求流式转发到已绑定上游，再把上游响应流式写回客户端：

- method、path、query 默认继承入站请求；`url` 非空时覆盖 path/query。
- 入站请求头会复制到上游，但 framing 和 hop-by-hop 头会过滤；`headers` 随后执行覆盖或删除。
- 请求体直接从入站流转发，`ProxyPassOptions` 没有独立 body 字段。
- 上游响应头过滤 hop-by-hop 字段后写回；`responseHeaders` 可覆盖，`null`/`undefined` 可删除。
- 响应体默认使用 64 KiB buffer 和 48 KiB low-water；不足 low-water 的数据会等待更多上游数据或 EOF。
- `flush: true` 关闭跨读取聚合：每次最多读取 64 KiB，当前块完整写出后才读取下一块。SSE、流式 JSON
  和其他低延迟分块响应应显式启用。该选项只影响本地 response body pipe，不会自动设置
  `X-Accel-Buffering: no`，也不会关闭外层代理或协议栈自身的缓冲。
- 成功返回上游状态码，但响应此时已经由函数发送。通常应 `return service.proxyPass({...});` 或让它成为脚本的最后一个有效动作，不要再调用 `resp.send*()`。
- `websocket: true` 支持把入站 HTTP/1.1 WebSocket Upgrade 或 HTTP/2/3 Extended CONNECT 转成上游 HTTP/1.1 Upgrade，并一直等待双向隧道结束。此模式强制上游 GET，显式非 GET method 会报错。
- WebSocket 模式会在自定义头覆盖之后重新确立握手必需字段，`timeout` 同时作为隧道单次读写超时。

## 7. C++ 嵌入 Script

### 7.1 CMake 链接

仓库内应用直接链接 `fiber_lib`；其 `src/` include 目录会以 PUBLIC 方式传递：

```cmake
add_executable(script_embed main.cpp)
target_link_libraries(script_embed PRIVATE fiber_lib)
```

### 7.2 编译并同步执行

```cpp
#include <cstdio>
#include <string>
#include <utility>

#include "script/JsGc.h"
#include "script/JsValue.h"
#include "script/ScriptCompiler.h"
#include "script/std/StdLibrary.h"

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
#include "script/Library.h"
#include "script/ScriptResult.h"
#include "script/std/StdLibrary.h"

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

#include "http_script/HttpScriptLib.h"
#include "http_script/ConstPackage.h"
#include "http_script/RouteScriptExtension.h"
#include "script/ScriptCompiler.h"
#include "script/std/StdLibrary.h"

struct ScriptRuntime {
    fiber::script::std_lib::StdLibrary library;
    fiber::http_script::RouteScriptExtension route_extension;

    ScriptRuntime() {
        fiber::http_script::register_http_functions_to_lib(library);
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
- `ConstPackage` 拥有常量 HostCallable 的 userdata，必须与使用它编译出的脚本一起存入快照并至少同寿命。
- `build()` 生成按 type 分区的紧凑 Entry/桶数组；每个分区使用不超过 50% 装载率的二次探测哈希。编译期的去重表、顺序表和 `HostCallable` 不进入不可变 package，package 只保留脚本 userdata 所需的稳定引用与归一化名称。
- `StdLibrary` 只参与编译；含 HTTP directive 的脚本仍要求 `RouteScriptExtension` 持有的 directive 定义比脚本活得更久，因为编译结果中的上游函数 userdata 指向这些定义。
- `CompileScope` 修改扩展的临时编译上下文，因此共享扩展时必须串行编译；脚本执行不读取这份可变状态。
- 只编译同步模板时应把 HTTP directives 关闭，并在编译后拒绝 `contains_async()`。
- 运行期绑定进常量槽的借用文本，以及 `ScriptConnectionInfo::scheme` 指向的文本，都必须持续到该次脚本执行结束；query 解码结果由 `prepare_constants()` 自动复制进请求池。

### 8.2 每请求执行

```cpp
#include <string_view>
#include <utility>
#include <vector>

#include "http_script/ScriptExchangeCtx.h"
#include "script/JsGc.h"
#include "script/Script.h"
#include "script/ScriptResult.h"

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

也可以把一组 `IndexedConstValue` 传给 `prepare_constants()` 或 `bind_constants()`。未设置的槽保持 `null`；脚本执行时 HostCallable 只校验 package identity 和 index，然后直接返回槽值，不再按名称扫描请求数据。

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

- 编译入口：`src/script/ScriptCompiler.h`
- 执行入口：`src/script/Script.h`、`src/script/ScriptResult.h`
- C++ 值/GC API：`src/script/JsValue.h`、`src/script/JsGc.h`
- 标准库注册：`src/script/std/StdLibrary.cpp`
- HTTP 函数：`src/http_script/RequestFuncs.cpp`、`ResponseFuncs.cpp`
- 上游 HTTP：`src/http_script/HttpClientFuncs.cpp`
- HTTP 执行上下文：`src/http_script/ScriptExchangeCtx.h`
- lite-nginx 脚本示例：`apps/lite_nginx/conf/scripts/`
- 完整 HTTP 执行参考：`apps/lite_nginx/src/runtime/ServerLauncher.cpp`
- 标准库行为测试：`tests/*FuncsTest.cpp`
- HTTP API 行为测试：`tests/HttpScriptFuncsTest.cpp`
