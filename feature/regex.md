# 正则引擎 `fiber::regex` 设计

状态：阶段 1–4 已实现（2026-11-04）；Prefilter 的启动条件未满足，没有做（§11）。范围：进程内线性时间正则
引擎，架构移植自 RE2，不引入任何外部依赖。WAF 本身（规则语言、变量集合、转换函数）不在本文范围，本文只定义
WAF 需要引擎提供什么。

## 1. 背景与目标

项目目前没有正则实现，也没有使用 `std::regex`。等着用的有两处：

| 使用方 | 现状 | 对引擎的要求 |
|---|---|---|
| 脚本 `strings.match` / `findAll` | 未注册，`src/script/std/StringsFuncs.h:14` 留了位置 | UTF-8 文本，模式可能运行时拼出 |
| WAF（可能） | 尚未开始 | 大规则集、字节语义、流式 body、抗 ReDoS |

WAF 的要求最严，引擎按它定范围，脚本是子集。

lite_nginx 不接入：路由继续由 `RoutePathMatcher` 负责，不加 `location ~` 和 `rewrite`，也不追求与 nginx
完全一致。

目标：

1. **线性时间**。匹配耗时对输入长度线性，模式写得再差也不会回溯爆炸。
2. **零外部依赖**。不引入 RE2，也不引入 Abseil。项目此前已为避开 Abseil 把 protobuf 钉在 v21.12
   （`cmake/Deps.cmake:139`）。
3. **能跑 OWASP CRS 规则**。这是语法和语义范围的验收基线。
4. **失败不静默**。资源耗尽绝不能表现为"未命中"，否则 WAF 会 fail-open。

## 2. 实测依据

下面是动手前的实测，决定了设计。基线：OWASP CRS bd1c42a（2026-11-04）、RE2 2da0056（2026-10-02）。实现后的
实测在 §18。

| 项 | 结果 | 设计结论 |
|---|---|---|
| `@rx` 规则 | 320 条（297 条不同） | 验收集 |
| Go `regexp/syntax`（RE2 语法）解析并编译 | 320 通过，0 失败 | RE2 语法够用 |
| 前瞻、后顾、反向引用、原子组、占有量词 | 0（CRS `CONTRIBUTING.md` 明确要求不用） | 不实现 |
| `\p{...}` | 0 | 首版不实现 |
| `\x{c2}\x{b4}` 这类写法 | 多条 SQLi / XSS 规则按原始字节书写 | 必须有字节模式 |
| `.` 的语义 | ModSecurity 用 DOTALL，CRS 文档也写明 | WAF 必须开 `dot_matches_newline` |
| `$` 的语义 | 56 条使用；ModSecurity 用 DOLLAR_ENDONLY | 与 RE2 默认一致，不用做兼容 |
| 模式长度 | 最长 12,541 字节，合计 213,179 字节 | 上限定 64 KiB |
| 最大有界重复 | 256 | RE2 的 1000 够用 |
| 程序规模 | 合计 115,270 条 Go 指令，中位 46，最大 6,566 | 见 §13 |
| `capture` 动作 | 279 处 | 捕获必须有，但只在命中后才用 |
| `@pmFromFile` / `@pm` | 19 / 4 处；20 个 `.data` 文件，trie 共 83,772 节点，最大 45,269 | 需要多字面量匹配器，且不能用稠密表 |
| 规则分组 | 按（phase、目标、转换链）115 组，66 组只有一条，最大 50 / 37 / 26 | Set 只能按组用 |
| 单变量上的规则数 | ARGS 最大两组 51 和 47 条，RESPONSE_BODY 50 条 | Set 收益集中在这里 |
| 可提取必含字面量 | 144/320（45%）；RCE 6/56，SQLi 16/61，XSS 20/30，RESPONSE-951/955 全部 | Prefilter 收益有限，见 §11 |

说明：

- Go 的结果只证明语法兼容，不证明匹配语义一致。比如 `\x{c2}` 语法上能过，但 UTF-8 模式下含义就错了。
- ModSecurity 的标志取自源码：v2 `apache2/re_operators.c` 和 v3 `src/utils/regex.cc`（非 multiLine
  路径）都是 `DOTALL | DOLLAR_ENDONLY`。
- 字面量覆盖率来自一个简化模型，不是 RE2 的 Prefilter：对每条正则求一个"至少出现其中之一"的小写字面量
  集合，`min_atom_len = 3`，没有 AND 树。它回答"能否过滤"，不回答选择性，真实数字可能略有出入。
- Go 指令按字符计，字节级程序的规模会有差异，只作量级参考。

RE2 里影响设计的三个事实：

1. **`RE2::Set` 没有 NFA 回退**。`dfa.cc:1436` 的注释写明 Set 不能回退；`Set::Match` 在 DFA 内存不足时
   返回 `false`，只靠可选的 `ErrorInfo::kOutOfMemory` 区分。调用方若不传它，结果就和"未命中"一样。
2. **`FilteredRE2` 不含字符串匹配引擎**（`filtered_re2.h:13`），多字面量匹配要调用方自己提供。
3. **RE2 不支持流式输入**。`Set::Match` 只接受一段连续的 `string_view`，`Consume` 也只是在连续字符串上前移。

## 3. 范围

| 类别 | 内容 |
|---|---|
| 实现 | RE2 语法（§4 列出的例外除外）、UTF-8 和字节两种模式、Pike VM、BitState、OnePass、Lazy DFA、RegexSet |
| RE2 没有但要加 | 流式 Scanner、多字面量匹配器 LiteralSet、不会失败的 DFA 退化路径、工作量预算 |
| 有条件才做 | Prefilter（§11） |
| 不做 | `\p{...}` 和 Unicode 大小写表、`mimics_pcre`、`tostring`、`longest_match` 公共选项、RE2 的类型转换模板 |

Unicode 表可以后续单独加，不影响架构：约 6,500 行生成的类别表和约 600 行大小写折叠表，RE2 的生成脚本可参考。

## 4. 语法和语义

### 4.1 支持的语法

字面量、`.`、字符类（含 POSIX 类和 `\d \s \w` 及其反义）、标准转义（`\t \n \x7F \x{10FFFF}`、八进制）、
`\Q...\E`、贪婪和懒惰量词、`{n,m}`、捕获组、`(?:...)`、命名组 `(?P<name>...)` 与 `(?<name>...)`、
标志 `(?imsU)` 和 `(?i:...)`、`^ $ \A \z \b \B`。

语义与 RE2 相同：leftmost-first，`\d \s \w \b` 只按 ASCII，`\s` 不含 `\x0b`。CRS 也因此写成 `[\s\x0b]`。

### 4.2 明确拒绝

以下模式在编译期返回错误（`ErrorCode::UnsupportedFeature`）并带上偏移量，不做静默降级：

- 反向引用、前瞻、后顾、原子组、占有量词、`\K`、条件分组：无法在线性时间内实现。
- `\C`：会破坏 UTF-8 模式下的匹配边界，字节模式下 `.` 已能覆盖。
- `\p{...}`：首版不支持。

### 4.3 两种编码

| | `Encoding::Utf8` | `Encoding::Bytes` |
|---|---|---|
| 模式和文本的单位 | Unicode 码点 | 字节，0–255 |
| `\x{c2}` | U+00C2，匹配字节 `C3 82` | 字节 `C2` |
| `\x{100}` | U+0100 | 编译错误 |
| `.` 和 `[^x]` | 只匹配合法 UTF-8 序列 | 匹配任意字节（`.` 默认除 `\n`） |
| `(?i)` | 首版只折叠 ASCII | 只折叠 ASCII（同 RE2 `AddFoldedRangeLatin1`） |
| 适用 | 脚本 | WAF |

**WAF 必须用 `Bytes`**，原因有两个：

- CRS 规则按字节书写。`\x{c2}\x{b4}` 是 `´` 的 UTF-8 字节序列，在 UTF-8 模式下会被读成两个别的字符。
- UTF-8 模式下 `.` 不匹配非法字节，攻击者插入一个坏字节就可能绕过 `a.*b` 这样的规则。

UTF-8 模式下 `(?i)` 只折叠 ASCII 是首版的有意简化，与 Java `CASE_INSENSITIVE` 的默认行为一致。

### 4.4 WAF 的标准选项

```cpp
Options{.encoding = Encoding::Bytes, .dot_matches_newline = true}
```

漏掉 `dot_matches_newline` 不会报错，但攻击者插入换行就能绕过 `.*`，所以 WAF 规则加载层要统一设置，
不交给单条规则。

## 5. 架构

```text
pattern
  │  Parser（显式栈，同 RE2，带深度上限）
  ▼
AST ──Simplify（展开 {n,m}、合并字面量）──▶ AST
  │  Compiler
  ▼
Program（前向 + 反向，只读，可跨线程共享）
  │
  ├─ Pike VM    线性，支持捕获，兜底
  ├─ BitState   带 visited 位图的回溯，短文本捕获
  ├─ OnePass    无歧义锚定模式的捕获
  └─ Lazy DFA   不产出捕获，最快；RegexSet 和 Scanner 的基础
```

文件布局沿用 `include/fiber/<module>` + `src/<module>`。`src/*.cpp` 由 `GLOB_RECURSE` 收集，不用改 CMake。

```text
include/fiber/regex/
  Regex.h  RegexSet.h（含 MatchSet、Scanner）  Scratch.h  LiteralSet.h
src/regex/
  Rune.h  Sparse.h  CharClass.{h,cpp}  Ast.{h,cpp}  Parser.{h,cpp}  Simplify.{h,cpp}
  Compiler.{h,cpp}  Program.{h,cpp}  Compiled.h
  Search.h  PikeVm.{h,cpp}  BitState.{h,cpp}  OnePass.{h,cpp}  Dfa.{h,cpp}
  Regex.cpp  RegexSet.cpp  LiteralSet.cpp  Scratch.cpp  ScratchState.h
  LICENSE.re2  UPSTREAM.md
```

依赖方向：`fiber::regex` 只依赖 `common`（`Assert.h`、`NonCopyable.h`、`mem::BufPool`），不依赖 `event`、
`async`、`http`。它是纯同步计算，调度和 `IoBufChain` 适配都留给调用方。

## 6. Program

指令格式沿用 RE2：8 字节一条，28 位 `out`、1 位 `last`、3 位 opcode。opcode 为 `Alt`、`AltMatch`、
`ByteRange`、`Capture`、`EmptyWidth`、`Match`、`Nop`、`Fail`。所有匹配都按字节进行，UTF-8 在编译期展开成
字节范围序列。

编译期完成：

- **Flatten**：把指令整理成链表头（`list_count`），DFA 状态只存链表头，BitState 的位图也按它计算。
- **ByteMap**：把 256 个字节压缩成等价类，DFA 的 `next` 表按等价类数量分配。
- **UTF-8 后缀共享**：大字符类展开时缓存公共后缀，控制指令数。
- **反向程序**：用于确定 `$0` 的起点。在 `compile()` 里一次建好，不用 RE2 那样的 `call_once` 懒加载，这样
  `Regex` 编译后完全只读。超过 `max_insts` 时只是没有反向程序，由 Pike VM 承担这部分工作，不算编译失败。
- **OnePass 表**：判定成立时在编译期构建。
- **前缀加速**：字面量前缀用 `memchr`。RE2 针对 `(?i)` 的 ShiftDFA 先不做，CRS 有 137 条 `(?i)`，
  阶段 3 看基准再定。

## 7. 引擎分发

| 调用 | 条件 | 路径 |
|---|---|---|
| `test` | 默认 | 前向 DFA，earliest match，命中立即返回 |
| `find`，只要 `$0` | 默认 | 前向 DFA 定终点，反向 DFA（longest）定起点 |
| `find`，要子捕获 | 文本短 | 跳过 DFA，直接 OnePass 或 BitState |
| `find`，要子捕获 | 其他 | 先用 DFA 定 `$0`，再在这段子串上跑 OnePass、BitState 或 Pike VM |

引擎适用条件沿用 RE2：

- OnePass：锚定、无歧义、捕获不超过 5 组（`prog.h:362`）。
- BitState：`list_count × (len + 1)` 不超过 256 Kbit（`prog.cc:656`）。
- Pike VM：无条件，兜底。

阶段 1、2 还没有 DFA，`test` / `find` 全走后三者，公共 API 不变。

## 8. DFA

整体沿用 RE2 的 Lazy DFA：状态是 NFA 指令集合加标志位，转移表按需填充。`kByteEndText` 作为文本末尾的虚拟
字节，`kFlagLastWord` 记录上一字节是否为单词字符，所以 `\b`、`$` 不需要回看文本。以下四点与 RE2 不同。

### 8.1 所有权：缓存归 worker，不加锁

RE2 的 DFA 挂在 `Prog` 上，靠 `RWLocker` 和 `std::atomic` 支持多线程共享。这里 `Program` 完全只读，DFA 缓存
放在调用方持有的 `Scratch` 里，每个 worker 一个：

- `next[]` 是普通指针，没有原子操作和读写锁。
- 状态内存从 `mem::BufPool` 分配，缓存重置就是一次 `BufPool::reset()`。
- `Scratch` 按 `ProgramId` 索引缓存。ID 是编译时从进程级原子计数器取的，不复用，所以 `Regex` 析构后的残留
  条目是安全的，由预算按 LRU 淘汰。
- **内存总量 = worker 数 × 预算，与规则条数无关**。RE2 的 `max_mem` 是每个 RE2 对象各算一份。

代价是每个 worker 各自预热一份缓存。

### 8.2 预算和退化：DFA 永不失败

RE2 的 DFA 在缓存抖动时返回失败，单个 RE2 退回 NFA，Set 则无路可退。这里改为三级，都能得到正确结果：

| 级别 | 触发 | 行为 |
|---|---|---|
| 缓存 | 默认 | 查表转移，每字节一次访存 |
| 淘汰 | 这个 Scratch 超预算 | 淘汰最久未用的其他缓存；只剩自己时重置自己，保留当前状态后继续 |
| 无缓存步进 | 距上次重置处理的字节数 < 10 × 缓存状态数（RE2 同款判据，`dfa.cc:1439`） | 用工作队列直接推进，不写缓存 |

无缓存步进本质上是 NFA 集合模拟，复用 DFA 的单步函数，只是跳过状态 intern，`ManyMatch` 同样可用，所以
RegexSet 也有了 RE2 没有的回退路径。退化只持续到本次调用结束，下次调用重新尝试缓存。

### 8.3 流式：状态可跨调用恢复

单次扫描的状态只有"当前 DFA 状态"，天然可以跨 chunk 延续：`feed()` 推进，`finish()` 喂入 `kByteEndText`。

难点在**协程交错**。同一个 worker 上多个请求的 body 交替到达，两次 `feed()` 之间缓存可能已被别的请求重置，
`State*` 会悬空。解决办法：

- `Scanner` 记录缓存的 epoch，并在每次 `feed()` 结束时把当前状态的指令列表和标志位存成快照。
- 下次 `feed()` 时 epoch 不一致，就用快照重新 intern。epoch 来自进程级计数器，缓存新建和重置时都换一个。不用
  指针比较：缓存被淘汰后，新缓存可能落在同一地址。

快照成本是每个 chunk 一次 O(状态大小) 的拷贝，相对 chunk 处理可以忽略。快照按需增长，理论上限是
`list_count × 4` 字节。

流式的限制：

- **只报告命中了哪些规则**，不给匹配位置和捕获，因为前面的字节已经不在了。WAF 需要记录匹配内容时，自己保留
  一个尾部窗口再重跑单条正则，内容可能不完整。
- 跨 chunk 的语义由测试保证：任意切分结果都必须和一次性扫描相同（§16）。

### 8.4 工作量预算和事件循环

线性时间不等于便宜。无缓存步进每字节的代价约等于当前状态大小，集合较大时 1 MB body 可能要数十亿次操作，
所以要限制总量，并且不能阻塞事件循环。

- **`WorkBudget`**：调用方持有，按请求计。扣减项是状态构建、无缓存步进、Pike VM 步进、BitState 访问，DFA 另外
  在扫描开始时按文本长度一次性扣减，这样命中缓存的扫描也计入预算，而内循环仍然没有任何检查。耗尽时返回
  `MatchStatus::BudgetExhausted`。
- **不阻塞循环**：`feed()` 是同步调用。调用方把 body 切成小片（初值 64 KiB），用
  `EventLoop::turn()` 计账，配额用完后 `co_await async::yield()`，也就是
  `include/fiber/event/EventLoop.h:141` 描述的做法。

`BudgetExhausted` 的处理（放行还是拦截）是 WAF 的策略，引擎只保证把它如实报告出来。

## 9. RegexSet

编译方式同 RE2：每条模式包一层 `HaveMatch(id)`，组成 `Alternate`，非锚定时前面加 `.*`（`Compiler::CompileSet`），
用 `ManyMatch` 模式跑 DFA。每经过一个匹配状态，就把状态里的 ID 累积进调用方的 `MatchSet` 位图。

与 RE2 的差别：

- 返回三态 `MatchStatus`，内存不足不会变成"未命中"（§8.2）。
- 同样支持 Scanner，ID 位图跨 chunk 累积。

**用法上的约束**：转换链不同的规则看到的文本不同，不能放进同一个 Set。CRS 按（phase、目标、转换链）分出
115 组，66 组只有一条。收益集中在大组：ARGS 上最大的两组 51 和 47 条，用两遍 DFA 就能代替 98 次单独匹配。

不要把所有规则合成一个 Set：实测状态数会爆炸（§18）。程序超过 `max_insts` 的 16 倍时 `build()` 报错，由调用方
拆组。

## 10. LiteralSet

Aho-Corasick 多字面量匹配器，服务 `@pm` / `@pmFromFile`，也是 Prefilter 的前置组件。RE2 没有这部分。

- **不能用稠密转移表**。最大的 `php-errors.data` 有 45,269 个 trie 节点，256 × 4 字节的表要 44 MiB。
  根节点和高扇出节点用稠密表，其余节点用排序边数组加 fail 链接。
- 大小写不敏感在建表时折叠，扫描时走 256 字节映射表。
- 匹配状态是一个 `uint32_t`，天然支持流式。
- 实现后 CRS 全部 20 个词表都在 4 MiB 以内，见 `RegexCrs.LoadsEveryWordList`。

## 11. Prefilter（有条件才做）

此前的判断是必做，实测后改为有条件。

Prefilter 的作用是先用字面量排除不可能命中的规则。但对 CRS 请求侧规则，它和 RegexSet 基本是替代关系：

- 只有 45% 的规则能提取必含字面量，攻击类规则更低（RCE 6/56、SQLi 16/61）。
- **一个 Set 里只要有一条不可过滤，这一遍 DFA 就省不掉**。而 RCE 和 SQLi 所在的组几乎必然包含这样的规则。
- 覆盖率 100% 的 RESPONSE-951/955 针对的是响应 body。流式路径上文本已经走了，字面量命中后没有东西可供
  重跑，所以 Prefilter 也用不上；只有把响应 body 缓冲下来才能用。

**启动条件**：CRS 基准显示字段扫描是 CPU 瓶颈，并且把可过滤规则移出 Set 能带来可测量的收益。否则不做。

**现状：没有做**。分组 Set 在短字段上的耗时主要是每次调用约 140 ns 的固定开销，而不是匹配本身（§18），Prefilter
省不掉这部分。

## 12. 公共 API

下面是摘要，完整定义以 `include/fiber/regex/` 为准。错误用独立的 `Error`，不用 `IoResult`：这不是 I/O 错误，而且
需要带偏移量，方便指出模式里的出错位置。

```cpp
namespace fiber::regex {

enum class Encoding : std::uint8_t { Utf8, Bytes };

struct Options {
    Encoding encoding = Encoding::Utf8;
    bool ignore_case = false;
    bool dot_matches_newline = false;
    bool multi_line = false;
    std::uint32_t max_insts = 1U << 16;
};

enum class ErrorCode : std::uint8_t {
    Syntax, UnsupportedFeature, InvalidUtf8, PatternTooLong,
    RepeatTooLarge, NestingTooDeep, ProgramTooLarge,
};

struct Error {
    ErrorCode code;
    std::uint32_t offset; // 模式内的字节偏移
};

struct Span {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);
    std::size_t begin = npos; // 未参与匹配的组保持 npos
    std::size_t end = npos;
};

enum class MatchStatus : std::uint8_t { NoMatch, Match, BudgetExhausted };

// 调用方按请求持有。不传时不限量。
struct WorkBudget {
    std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max();
};

// 每个 worker 一个，非线程安全。只在同步调用内使用，不跨 co_await 保存内部指针。
class Scratch : public common::NonCopyable, public common::NonMovable {
public:
    explicit Scratch(std::size_t dfa_budget_bytes = kDefaultDfaBudgetBytes); // 32 MiB
    [[nodiscard]] std::size_t dfa_bytes_used() const noexcept; // 用来按真实流量定预算
};

// 编译后只读，可跨线程共享，拷贝只增加引用计数。
class Regex {
public:
    [[nodiscard]] static std::expected<Regex, Error> compile(std::string_view pattern,
                                                             const Options &options = {});

    [[nodiscard]] std::uint32_t capture_count() const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> capture_index(std::string_view name) const noexcept;

    [[nodiscard]] MatchStatus test(std::string_view text, Scratch &scratch) const noexcept;
    [[nodiscard]] MatchStatus test(std::string_view text, Scratch &scratch,
                                   WorkBudget &budget) const noexcept;

    // captures[0] 是 $0。captures.size() 决定要算几组，长度 1 时不走子捕获阶段。
    // start 之前的字节仍作为上下文，保证 ^ 和 \b 判断正确，findAll 靠它推进。
    [[nodiscard]] MatchStatus find(std::string_view text, std::size_t start,
                                   std::span<Span> captures, Scratch &scratch) const noexcept;
};

class MatchSet; // 位图，大小等于规则数，调用方持有

class RegexSet {
public:
    class Builder {
    public:
        explicit Builder(const Options &options);
        [[nodiscard]] std::expected<std::uint32_t, Error> add(std::string_view pattern);
        [[nodiscard]] std::expected<RegexSet, Error> build() &&;
    };

    [[nodiscard]] MatchStatus scan(std::string_view text, MatchSet &hits, Scratch &scratch,
                                   WorkBudget &budget) const noexcept;
};

// 一个 body 流对一个 RegexSet。归请求所有，可以跨 co_await 存活。
// 单条正则的流式匹配就是一个元素的 RegexSet。
class Scanner {
public:
    explicit Scanner(const RegexSet &set);

    [[nodiscard]] MatchStatus feed(std::string_view chunk, Scratch &scratch,
                                   WorkBudget &budget) noexcept;
    [[nodiscard]] MatchStatus finish(Scratch &scratch, WorkBudget &budget) noexcept;
    [[nodiscard]] const MatchSet &hits() const noexcept;
};

} // namespace fiber::regex
```

`Scratch` 每次调用都显式传入，不放进 `Scanner`，这样 `Scanner` 里没有悬空引用的风险；如果请求换了 worker，
§8.3 的 epoch 校验会按"缓存已失效"处理。

匹配路径遵守项目的热路径约定：`noexcept`，不用 `std::string` / `std::vector` / `std::function`。每个 Program
在每个 Scratch 上首次使用时分配工作区，之后除 DFA 状态外不再分配。编译路径是冷路径，可以用标准容器。

## 13. 资源上限

| 项 | RE2 | 本项目 | CRS 实测 |
|---|---|---|---|
| 模式长度 | 无 | 64 KiB | 12,541 字节 |
| 单次重复 `{n,m}` | 1000 | 1000 | 256 |
| 嵌套重复展开积 | 1000（`RepetitionWalker`） | 1000 | Go 全部通过 |
| AST 深度 | 无上限（非递归 Walker） | 1000（同 Go `maxHeight`） | Go 全部通过 |
| 单条 Regex 指令数 | 由 `max_mem` 推导 | 默认 65,536 | 最大 6,566（Go 指令） |
| RegexSet 指令数 | 由 `max_mem` 推导 | `max_insts` 的 16 倍，默认 1,048,576 | 全部 320 条也放得下 |
| OnePass 捕获组 | 5 | 5 | — |
| BitState 位图 | 256 Kbit | 256 Kbit | — |
| DFA 内存 | 每个 RE2 默认 8 MiB，前向 2/3、反向 1/3 | 每 worker 一份，默认 32 MiB | 分组 Set 工作集 27 MiB |

化简和编译用递归实现，靠深度上限防栈溢出，省掉 RE2 的 Walker 框架。CRS 全部能通过这个上限。

这些上限在脚本场景下尤其重要，因为模式可能是运行时拼出来的，`(a{1000}){1000}` 这类模式必须在编译期被拒绝。

## 14. 依赖和许可证

RE2 核心代码用了 357 处 Abseil，其中 241 处是 `string_view`，全部有现成替代：

| Abseil | 替代 |
|---|---|
| `string_view`、`Span` | `std::string_view`、`std::span` |
| `ABSL_LOG`、`ABSL_CHECK` | `FIBER_ASSERT`、`FIBER_PANIC`；运行时不打日志 |
| `StrFormat`、`FPrintF` | 基本是调试输出，不需要 |
| `Mutex`、`call_once` | 不需要（§6、§8.1） |
| `flat_hash_map` / `flat_hash_set` | 编译期用 `std::unordered_*`；DFA 状态表自写开放寻址 |
| `FixedArray`、`InlinedVector` | `std::vector`（编译路径）或 Scratch 工作区 |

RE2 对 Threads 的依赖随锁一起消失，ICU 本来就是可选的。

**许可证**：实现会紧贴 RE2 的结构，按派生代码处理，沿用 zlib 的做法（`src/compression/LICENSE.zlib` 和
`UPSTREAM.md`）：放 `src/regex/LICENSE.re2`（BSD-3-Clause），`UPSTREAM.md` 记录参考的 commit 和偏离点。

## 15. 接入

### 15.1 脚本

- `strings.match` 是部分匹配：文本任意位置命中就返回 `true`，要全串匹配就在模式里写 `^...$`。
- `strings.findAll` 返回所有不重叠匹配的文本。紧跟在上一个匹配之后的空匹配不算，与 Go 一致。
- 编码用 `Utf8`。沿用字符串 API 的约定（`docs/script-guide.md:490`）：类型错误或含孤立代理项时按软失败返回
  `null`，所以引擎不用处理 WTF-8。非法模式同样按软失败处理。
- 如果以后要返回位置，用现有的 `byte_to_u16_index` 转成 UTF-16 下标。
- 编译结果做每线程 LRU（128 条，按模式字符串索引，`src/script/std/RegexCache.h`），否则每次调用都要重新编译。
  编译失败也缓存。`Scratch` 也放在 `thread_local`，`RandFuncs.cpp` 已有同样做法。

### 15.2 WAF

引擎对 WAF 的约定：

| 场景 | 路径 |
|---|---|
| 字段（header、参数、URI） | 文本连续且短，按（phase、变量、转换链）分组，`RegexSet::scan` |
| 大 body | `HttpBodyPipeReader::read` 拿到 `IoBufChain`，经 `first_readable()` 逐段喂给 `Scanner` |
| `@pm` / `@pmFromFile` | `LiteralSet` |
| 规则命中后要捕获 | 对命中的单条 `Regex` 重跑 `find`，只发生在命中路径上 |
| `BudgetExhausted` | WAF 策略决定放行或拦截，必须显式配置 |

## 16. 测试

| 层 | 内容 | 位置 |
|---|---|---|
| 单元 | 解析、限制、语义、Set、Scanner、LiteralSet | `tests/Regex*Test.cpp` |
| Golden | RE2 `search_test.cc` 的用例表（RE2 的 6 种标志变体）加随机模式，期望结果由 Go `regexp` 离线生成 | `RegexGoldenTest`、`scripts/regex/gen_golden.go` |
| 跨引擎 | 同一份 golden 分别强制走 Pike VM、BitState、OnePass、DFA 和派发器 | `RegexGoldenTest` |
| 退化路径 | 同一份 golden 在 2 KiB 和 0 字节 DFA 预算下重跑，结果必须不变 | `RegexGolden.DfaUnderMemoryPressure`、`DfaWithoutMemory` |
| 流式等价 | 任意切分与一次性扫描相同；两个流共用缓存时中途重置 | `RegexSetTest`、`regex_stream_fuzzer` |
| CRS 回归 | 320 条规则 × 1,500 个真实攻击 payload 对比 Go，词表全部加载 | `RegexCrsTest`、`tests/support/third_party/crs_bd1c42a/` |
| Fuzz | `regex_parse_fuzzer`、`regex_engines_fuzzer`、`regex_stream_fuzzer` | `fuzz/` |

要点：

- RE2 的用例表不带期望结果，它靠内部回溯器当 oracle，所以用 Go。Go 不进构建依赖：golden 是生成后提交的。
- Go 没有字节模式，生成 golden 时把模式和文本按 Latin-1 转成 UTF-8，再把偏移映射回来。Go 的 `(?i)` 会折叠
  U+00E0 这类字符，所以 `(?i)` 加高位字节的组合不做比较。
- 跑通 golden 不够，还要确认被测路径真的执行了：实现时用临时计数器核对过每个引擎和退化路径的调用次数。

基准：`example/regex_crs_bench.cpp`，用法见文件头。

## 17. 实现结果

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | Parser、Simplify、Compiler、Program、Pike VM、API | 完成 |
| 2 | BitState、OnePass；`strings.match` / `findAll` | 完成 |
| 3 | DFA、RegexSet、Scanner、预算 | 完成 |
| 4 | LiteralSet | 完成 |
| 5（条件） | Prefilter | 未做，见 §11 |

实现共 6,014 行有效代码（不含注释和空行），测试和 fuzz 1,457 行，另有生成的 golden 数据。动手前估算实现 6,200
行、测试 4,500 行：实现很准，测试少得多，因为大量用例由 Go 生成，成了数据。

与 RE2 的对照，同时也是阅读索引：

| 组件 | RE2 文件 | RE2 有效行 | 本项目 |
|---|---|---|---|
| 语法解析、AST | `parse`、`regexp`、`perl_groups`、`tostring` | 3,269 | `Parser`、`Ast`、`CharClass` |
| 化简 | `simplify` | 507 | `Simplify` |
| 编译器和程序 | `compile`、`prog` | 1,988 | `Compiler`、`Program` |
| Pike VM | `nfa` | 495 | `PikeVm` |
| 公共 API 和派发 | `re2` | 1,483 | `Regex` |
| BitState、OnePass | `bitstate`、`onepass` | 611 | `BitState`、`OnePass` |
| DFA、Set、预算 | `dfa`、`set` | 1,459 | `Dfa`、`RegexSet` |
| Scanner | 无 | 0 | `Dfa`、`RegexSet` |
| LiteralSet | 无 | 0 | `LiteralSet` |

## 18. 实现后的实测和遗留问题

### 18.1 正确性

| 验证 | 规模 | 结果 |
|---|---|---|
| 随机模式对比 Go | 116 万用例 × 7 种引擎和预算配置，ASan + UBSan | 零分歧 |
| CRS 规则 × 攻击 payload，含捕获偏移 | 479,589 对，28,014 次命中 | 与 Go 零差异 |
| 逐条、分组 Set、单个大 Set 三种方式的命中数 | 3,915 个 payload | 都是 73,724 |

差分用例在开发中抓到过一个真 bug：`(?i)\n` 这类模式让 OnePass 分析堆溢出。移植时把有符号的 `Rune` 换成了
`uint32_t`，小于 32 的字节在折叠计算时下溢。RE2 里同一表达式是负数，循环不执行。

### 18.2 性能

数据来自 WSL2，payload 平均 56 字节。每一遍都把全部规则跑在全部 payload 上，真实请求不会这样，所以看相对值。

| 方式 | 16 MiB 预算 | 64 MiB 预算 | DFA 内存 |
|---|---|---|---|
| 逐条跑 320 条 | 1.5 MB/s | 1.5 MB/s | 9.9 MiB |
| 114 个分组 Set | 0.41 MB/s | 3.3 MB/s | 27 MiB |
| 全部 320 条一个 Set | 0.06 MB/s | 0.08 MB/s | 256 MiB 预算下 120 MiB，16.4 MB/s |

结论：

1. **默认预算定为 32 MiB**。分组 Set 的工作集是 27 MiB。低于工作集时掉得很厉害，比逐条跑还慢：多个 Set 循环
   访问正好是 LRU 的最坏情况。
2. **不要把所有规则合成一个 Set**。状态数爆炸，恶意文本下 4 MiB 要 146 秒（0.03 MB/s）。
3. **DFA 比 Pike VM 快约 5 倍**（CRS 全量 `find` 0.13 s 对 0.64 s）。命中缓存的长文本扫描约 700 MB/s。
4. **短字段上固定开销占主导**：分组 Set 每次调用约 140 ns，几乎都不是匹配本身。

恶意文本（256 KiB，由攻击片段拼成，32 MiB 预算，冷启动）下，全部 114 组共 320 ms，最慢一组 144 ms（1.8 MB/s）。
`WorkBudget` 设为 1M 时在 87 ms 截断并返回 `BudgetExhausted`。

### 18.3 遗留问题

| # | 问题 | 现状 | 何时定 |
|---|---|---|---|
| 1 | 真实流量下的 DFA 工作集 | 27 MiB 来自回归语料，不是线上流量。`Scratch::dfa_bytes_used()` 可以观测 | WAF 上线时 |
| 2 | 高并发下 Scanner 快照的实际内存 | 按需增长，未测 | WAF 立项时 |
| 3 | `(?i)` 的前缀加速是否值得做 | CRS 有 137 条 `(?i)`，但 Set 路径用不到前缀加速，收益只在逐条匹配 | 有需求时 |
| 4 | 短字段的固定开销能否降低 | `DfaRun` 的构造和按 `Program` 查缓存是主要部分，未优化 | 基准显示它是瓶颈时 |
| 5 | 脚本是否需要 `\p{...}` 和 Unicode `(?i)` | 首版不要，有需求再加生成表 | 有需求时 |
| 6 | `BudgetExhausted` 时默认放行还是拦截 | WAF 设计时定，引擎不设默认 | WAF 立项时 |
| 7 | `WorkBudget` 的单位 | 状态构建比步进贵得多，所以同样的单位对应的时间差别较大 | WAF 定预算时 |
