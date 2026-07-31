# 现网 condition/template/rewrite corpus 差分

## 1. 范围与数据边界

本轮以 2026-07-31 导出的测试环境完整配置图作为现网语法快照，只比较
`ploto-unified-access` 实际使用到的 condition、template 和 rewrite 行为：

- Java 参考实现：
  `RouteExecutionBuilder.compileExpression/parseTemplate`、
  `ScriptStringTemplate` 和 `ConditionalExecution`；
- C++ 实现：
  `AccessScriptRuntime`、`TemplateEvaluator`、compiled route snapshot 和请求执行计划；
- 不把 Nacos 地址、账号、密码、项目名、Host、业务 header value、表达式字符串常量
  或完整 route JSON 写入 Git；
- 原始 dump 和 Java 探针产物只保留在被忽略的 `temp/` 中。

这仍然是有限的现网 corpus 兼容，不扩大为 Java/C++ 通用脚本 VM 等价承诺。

## 2. 快照覆盖

| 项目 | 数量 |
| --- | ---: |
| 项目 route 配置 | 352 |
| RouteItem | 1302 |
| condition | 11 |
| 含表达式的 template value | 810 |
| template expression segment | 811 |
| condition + template expression segment | 822 |
| rewrite string | 330 |
| 其中含表达式的 rewrite | 311 |
| 纯字面量 rewrite | 19 |
| 由 Jackson scalar coercion 得到的 rewrite | 1 |

810 个含表达式 template value 的位置分布：

| 位置 | 数量 |
| --- | ---: |
| `proxy_headers` | 482 |
| `response_headers` | 10 |
| `context` | 6 |
| `rewrite` | 311 |
| TEMPLATE response body | 1 |

实际语法集中在：

- `$path.tail/tail2/t/env`；
- `$header.host/hi_trace_cluster/origin/...`；
- `$context.hi_trace_cluster`；
- `$req.path/query/method`；
- `$query.redirect`、`$cookie.cluster`；
- `||` 默认值、`==`/`!=`/`<=`；
- `strings.hasPrefix` 和 `rand.random`。

所有 `$path.*` 引用都能在各自 route pattern 中找到对应 capture；现网 template
没有反斜杠转义、未闭合表达式或异步表达式。

## 3. 差分方法

### 3.1 全量配置接受结果

`ProductionScriptCorpusTest.CompilesExternalSnapshotWhenProvided` 从外部目录读取 route
JSON，逐份执行 C++ wire decode、route matcher 构建和脚本预编译。测试只报告配置序号
与结构化错误，不输出文件名或配置内容：

```bash
ACCESS_SERVER_SCRIPT_CORPUS_DIR=/path/to/private-dump/routes \
./build/apps-build/access-server/fiber_access_server_tests \
  --gtest_filter=ProductionScriptCorpusTest.CompilesExternalSnapshotWhenProvided
```

未设置 `ACCESS_SERVER_SCRIPT_CORPUS_DIR` 时该测试跳过，因此 CI 不依赖私有数据。

本快照结果：352/352 配置完成 decode 和 compiled snapshot 构建。

### 3.2 Java golden 与请求级执行

基于上述语法集合构造脱敏固定输入，由 Java 参考类执行 14 个 template case 和 5 个
condition case。随后使用相同变量和值执行 C++ 请求级用例：

- `AccessRequestHandlerTest.MatchesProductionConditionAndTemplateCorpus`；
- `AccessRequestHandlerTest.MatchesProductionRewriteCorpus`。

随机表达式使用 `rand.random(1) <= 0` 固定结果，只验证函数调用、数字比较和 condition
truthiness；不比较 Java/C++ PRNG 序列。

## 4. 差异与修复

首次差分发现 `$context.hi_trace_cluster` 不兼容：

- Java `ConstPackage` 对变量名执行 ASCII 大小写折叠，并把 `-` 归一为 `_`，所以表达式
  key `hi_trace_cluster` 能读取运行时 `HI-TRACE-CLUSTER`；
- C++ 原实现只接受 `cluster`、`HI_TRACE_CLUSTER` 和 `HI-TRACE-CLUSTER` 的精确拼写，
  导致现网 36 个 `$context.hi_trace_cluster` 引用得到 null，并错误使用 `||` fallback。

C++ 现已在 access-server 的 context lookup 边界执行同等归一化，不改动共享脚本 VM。
修复后 Java golden、condition/template 请求结果和两类 rewrite 结果一致。

## 5. 当前结论

- 现网快照的全部 condition/template/rewrite 可被 C++ 配置入口接受并预编译；
- 已观察到的变量、默认值、比较、prefix 判断、template 文本转换和 rewrite URI 行为一致；
- 编译失败仍在候选快照发布前 fail closed，并保留上一成功版本；
- 后续配置新增未覆盖的 namespace、函数或转义形态时，必须先更新脱敏 corpus 和 golden，
  不能仅以“脚本引擎不同”跳过差分。
