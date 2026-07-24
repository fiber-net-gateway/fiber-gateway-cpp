# ai-server JSON 字段抽取与改写设计

## 1. 目标

LLM 请求正文最大为 4 MiB。入口需要在不构造完整业务 DTO 的前提下：

- 一次遍历抽取路由所需的少量字段；
- 按任意已编译路径替换已有 JSON value；
- 保留完整原始正文，供 Provider 重试重复使用；
- 每次 Provider 尝试独立替换已有的 `model`，并按需替换已有的 `stream`；
- 保留未知字段、字段顺序、空白、数字和字符串的原始词法表示；
- 支持将来由配置快照提供的自定义 JSON 路径，而不在请求热路径编译路径。

实现基线是 Java `ploto-llm` 的 `JsonPathTree`、`JsonPathExtractor`、
`JsonPathModifier`、`LlmBodyExtractor` 和 `LlmBodyModifier`。C++ 版本保留其
路径和缺失字段语义，但采用原始字节区间改写，避免 Jackson 式整文档重写。

## 2. 路径契约

`JsonPathProgram` 支持当前 Java 业务所需的确定性子集：

| 语法 | 含义 |
| --- | --- |
| `$` | 根值 |
| `.name` | 对象固定字段 |
| `[17]` | 数组固定下标 |
| `.*name` | 对象任意字段，并把字段名捕获为 `name` |
| `[*name]` | 数组任意下标，并把十进制下标捕获为 `name` |
| `[*]` | 数组任意下标，不保存捕获值 |

路径在配置或静态初始化边界编译为不可变前缀树。编译阶段拒绝：

- 非 `$` 根路径、空字段、非法下标和尾随字符；
- 同一路径重复注册；
- 一个节点同时作为终点和其他路径前缀；
- 同层固定分支和通配分支冲突；
- 同一通配节点使用不同捕获名。

匹配值允许是标量、对象或数组。遇到与后续路径不匹配的中间值时，该分支视为
未命中，JSON 其余部分仍需完整校验。

## 3. 请求期数据流

```text
immutable JsonPathProgram
             |
             v
raw IoBuf -> JsonParser 单次遍历 -> action callback
   |                              |-> 拷贝命中的解码字符串到 request BufPool
   |                              |-> 记录 model/stream 原始值区间
   |                              `-> 记录通配捕获作用域
   |
   `---------------------------> ParsedLlmBody 持有原始 IoBuf
                                      |
                                      +-> attempt A: 原文切片 + model A
                                      +-> attempt B: 原文切片 + model B
                                      `-> attempt C: 原文切片 + model C
```

`JsonParser::current_end_offset()` 暴露当前 token 的原始、排他结束偏移。容器值的
结束偏移由匹配遍历在读到对应 `}` 或 `]` 后确定。因此回调同时获得：

- 解码后的标量值；
- 原始 JSON 值区间 `[begin, end)`；
- 编译期 action ID；
- 当前通配捕获链。

解析期间只有命中的字符串和小数组索引写入请求 `BufPool`。未命中的正文不复制、
不构造 DOM。

通用 `rewrite_json_paths` 在同一次 visitor 遍历中调用 function-pointer
rewriter。回调可以保留原值或返回一个已经编码的 JSON value；框架会校验
replacement 是且仅是一个完整 JSON value，再用 `IoBuf` slice chain 拼接结果。
路径树禁止父子终点冲突，运行期仍会防御区间倒序、重叠和越界。

## 4. ai-server 固定抽取程序

OpenAI Chat Completions：

- `$.model`
- `$.stream`
- `$.metadata.route_key`
- `$.metadata.routeKey`
- `$.prompt_cache_key`
- `$.messages[*].role`
- `$.messages[*].content`

Anthropic Messages：

- `$.model`
- `$.stream`
- `$.metadata.route_key`
- `$.metadata.routeKey`
- `$.container`
- `$.system`
- `$.messages[*].role`
- `$.messages[*].content`

`model`、路由键、角色和协议专有键只接受 JSON string 或 null；`stream` 只接受
boolean 或 null。消息 content 和 Anthropic system 的对象、数组值按 Java 轻量
抽取语义记为 null，不递归提取内容块。

`metadata.route_key` 的非空值优先于 `metadata.routeKey`。重复 JSON 字段保持输入
顺序：抽取结果采用最后一次匹配值，所有 `model`/`stream` 原始区间都会进入改写表。

## 5. 改写契约

`ParsedLlmBody::rewrite` 始终从保存的原始正文生成一次新的 `IoBufChain`：

1. 用 JSON generator 编码上游模型名并校验 UTF-8；
2. 按记录顺序保留原文片段；
3. 在 `model` 区间插入编码后的上游模型；
4. 仅当调用方提供 stream 覆盖值时替换已有 `stream`；
5. 追加剩余原文并标记 chain 完成。

明确语义：

- 原文没有 `model` 或 `stream` 时不插入字段；
- 重复字段全部替换，避免上游 first-wins/last-wins 差异绕过；
- 两次 Provider 尝试互不依赖，第二次不会以第一次改写结果为输入；
- 未改写部分字节级保持不变；
- 区间重叠、越界和无效 UTF-8 replacement 都显式失败；
- 原始正文由 `ParsedLlmBody` 独占一个引用，输出切片通过 `IoBuf` 引用计数保活。

与 Java 的有意差异是：Java modifier 会重新序列化整个 JSON，C++ 只拼接原文区间。
这不改变 JSON 语义，并进一步保证未知扩展字段的词法表示不丢失。

通用一次性修改直接使用 `rewrite_json_paths`。ai-server 的 Provider 重试使用
`ParsedLlmBody` 专用快路径：第一次解析已记录 patch site，后续尝试不再遍历 JSON，
只编码本次 replacement 并拼接原文 slice。

## 6. 自定义字段扩展

任意静态或调用方提供的路径现在都可以使用同一 program、visitor 和 rewriter。
若后续把字段规则加入 Nacos（当前 Java 配置契约没有这类外部 schema），必须遵循：

- Nacos 配置解析阶段把表达式和业务 action 编译成不可变 `JsonPathProgram`；
- 发布的 `LlmConfigSnapshot` 持有完整 program，新请求原子切换；
- 请求期只执行 program，不解析表达式、不分配路径节点；
- action 用整数 ID 分派，热路径不使用 `std::function`；
- 自定义改写仍只允许替换已有值，不隐式创建父对象或数组；
- 配置冲突或非法路径使该次配置更新失败并保留上一快照。

## 7. 验证范围

通用层测试覆盖固定字段、数组下标、对象/数组通配捕获、转义字段名、容器值区间、
重复字段、任意路径改写、非法 replacement、路径冲突、类型不匹配和非法 JSON。

ai-server 测试覆盖两种协议抽取、路由字段优先级、复杂 content、严格类型检查、
未知字段词法保持、缺失字段不插入、重复字段全部替换、每次尝试独立改写、
原始正文不变以及 replacement UTF-8 校验。
