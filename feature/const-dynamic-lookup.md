# 脚本常量动态查找重设计(ConstPackage / ScriptExchangeCtx)

状态:已实施,未提交(2026-11-06)。实施时修正 D2:Path 为**末个**胜 —— 旧
`bind_path_constants` 逐对无条件覆盖,重复名末对胜出;文档原记"首个胜"有误,代码按实际旧语义实现。
动机:一个请求会执行多个脚本(rewrite 模板、set_header 模板、script_file、access_log 模板),
且配置热更后请求中途可能换到新编译的脚本/新 ConstPackage。当前 `ScriptExchangeCtx` 与单个
`ConstPackage` 身份锁死(`prepare_constants` 二次调用 Invalid、`constant()` 身份不匹配
abort InvalidState),一个 ctx 只能服务一个包;lite_nginx 被迫每阶段新建 ctx + GcHeap,
重复做 header 扫描 / query 解码 / cookie 解析。

## 1. 目标与非目标

目标:
1. Constant 执行时由 ctx 按需解析:编译产物携带 `(type, normalized_name, hash)`,
   运行期 `ctx->resolve_constant(type, name, hash)` 动态查找 + 记忆化。
2. 彻底删除 ctx↔package 身份耦合:`prepare_constants / bind_constant(s) /
   bind_path_constants / clear_constants / constant() / IndexedConstValue / identity()`
   全部移除。
3. 删除 ConstPackage 侧只为预填槽服务的数据结构(`Entry / entries() / find() /
   buckets / ConstIndex`)。
4. 保留编译期存在性检查(`RouteScriptExtension::resolve_constant` nullptr → 编译报错,
   含 `$path.<name>` 必须在路由 pattern 内的 headline 检查)。

非目标(本期不做):
- 单请求单 ctx / 单 heap 的共享化(见 §10 follow-up;本期完成后 ctx 构造已接近零成本,
  共享化是纯收益优化,独立成片)。
- `$req` / `$conn`(ExchangeConstExtension 直读,不占常量通道)、`query()/headers()/cookies()`
  惰性视图、响应状态机:全部不动。
- VM / Compiler / `Library::HostCallable` ABI:`SyncConstant` + userdata + 函数指针不变,
  `InterpreterVm`、`Compiler.cpp` 零改动。

## 2. 语义决策(拍板)

| # | 决策 | 理由 |
|---|------|------|
| D1 | miss(请求中不存在)→ `success(null)`,与今天槽默认 Null 一致 | 宽松;与 `$query.x` 缺失语义连续 |
| D2 | 重复键语义逐类型保持现状:Header **首个**胜、Query **末个**胜、Cookie **首个**胜、Path **末个**胜(bind 逐对无条件覆盖) | 见旧实现 prepare/bind 各分支;重写取值路径最易无声漂移,逐条钉死 |
| D3 | Header/Cookie/Path 值**借用**请求缓冲,native string 不拷贝;Query 值 percent-decode 输出**拷入** exchange pool | 与今天 set_string 的 copy 参数逐处一致(Header false / Query true / Cookie false / Path false) |
| D4 | `$context`(ConstType::Context)与 `external_values` 死接口删除 | 全仓无绑定方、无 `$context` 使用者;生产路径恒 null |
| D5 | OOM 时机从"脚本前 500 SCRIPT_CONSTANTS"变为"首次解析时 abort(OutOfMemory) → 500 + abort_reason_name" | 常量按需物化的必然代价;错误体字符串变化,记入行为变化清单 |
| D6 | 跨代混跑(脚本来自包 A、匹配路由来自位置 B)时 `$path.x` 查不到 → null,不是错误 | 今天这条路径是 500(身份不匹配);新语义显式定义为宽松,配置换代的阶段边界混用本已可能 |
| D7 | 缓存值只允许 native string / null,不允许 GC 引用 | 无需 GC 根化,OOM 语义与今天一致;不要"顺手"统一到 query()/headers() GC 对象(Header 对象存原始大小写带 `-`,规范化键不匹配) |
| D8 | 请求期内不失效 | 现无任何中途改写 exchange 请求态的调用方(全仓无 set_request_uri/rewrite_request);若未来引入,须在改写点加显式失效钩子并注释在此 |
| D9 | 缓存 `name` 维持借用包 Ref,契约显式化:**脚本及其 ConstPackage 必须活过它执行所针对的每一个 ScriptExchangeCtx**;已否决两个后续提议(2026-11-06 复审) | ①Header 常量 O(1) 化(`_`→`-` 转化走 `HttpHeaders::get`):漏真名带下划线的头、混合 `-_` 头、重复头 first-wins→last-wins 翻转(桶链前插),语义坑大于收益,且 memo 后每 distinct 常量每请求只扫一次,绝对收益 ~百 ns;②name 拷入 pool / 复用 header 指针:拷贝不修任何可达 bug 且新增 OOM 分支,header 指针则规范化键与 lowcase 名字节不一致会令 memo 永不命中。契约以注释落在 `ConstantEntry`、`resolve_constant`、`ConstPackage` 类注释三处 |

## 3. 新 API

### 3.1 ConstPackage.h

```cpp
enum class ConstType : std::uint8_t { Path = 0, Query, Header, Cookie, Count };
// ConstType::Context 删除;ConstIndex / kInvalidConstIndex / Entry / find() / entries() /
// size() / identity() 删除。

// 规范化键的唯一事实源(现 ConstPackage.cpp 匿名命名空间实现,提为公共函数)。
// 包侧(编译期)与 ctx 侧(运行期缓存)禁止各自另抄一份。
[[nodiscard]] std::uint64_t const_name_hash(std::string_view name) noexcept;   // FNV-1a 64,逐字节 normalize(小写 + '-'→'_')
[[nodiscard]] bool const_name_equal(std::string_view normalized, std::string_view probe) noexcept;
```

`Ref`(包内布局,userdata 指向它):

```cpp
struct Ref {
    std::uint64_t hash = 0;          // const_name_hash(normalized_name),add 时算好
    std::uint32_t name_size = 0;
    bool sealed = false;             // build() 置位;constant_fn 的哨兵(取代 owner/index 判空)
    ConstType type = ConstType::Count;
    // char name[] 紧随其后(normalized 字节,包生命周期稳定)
};
```

`constant_fn` 改为:

```cpp
AbiResult ConstPackage::constant_fn(void *userdata, const HostCallFrame &frame) noexcept {
    const auto *ref = static_cast<const Ref *>(userdata);
    auto *context = static_cast<ScriptExchangeCtx *>(frame.attach);
    if (ref == nullptr || !ref->sealed || context == nullptr) {
        return AbiResult::abort(ScriptAbortReason::InvalidState);
    }
    return context->resolve_constant(ref->type, std::string_view{ref_name(ref), ref->name_size}, ref->hash);
}
```

### 3.2 ScriptExchangeCtx.h

```cpp
// 路由匹配后、脚本运行前由宿主注入捕获(名/值均借用匹配文本与请求路径缓冲)。
void set_path_vars(std::span<const std::pair<std::string_view, std::string_view>> vars) noexcept;

// ConstPackage 编译产物(SyncConstant callable)的运行期入口。
// name 为包侧规范化键,hash 为 const_name_hash(name) 的编译期预计算值。
[[nodiscard]] fiber::script::AbiResult
resolve_constant(ConstType type, std::string_view name, std::uint64_t hash) noexcept;
```

删除:`prepare_constants / bind_constant / bind_constants / bind_path_constants /
clear_constants / constant / IndexedConstValue / const_package_identity_ /
constant_slots_ / constant_slot_count_`;类头注释第 2 条"indexed dynamic-constant frame"
改写为"memoized on-demand constant resolution"。`friend ExchangeConstExtension` 保留
(仍直读 `exchange_ / connection_ / remote_address_constant_`)。

### 3.3 私有缓存(ctx 内)

```cpp
struct ConstantEntry {
    std::uint64_t hash = 0;                  // 探测键
    std::string_view name;                   // 借自包 Ref(规范化);data()==nullptr 即空槽
    fiber::script::JsValue value{};          // native string 或 null,绝无 GC 引用
};
std::span<const std::pair<std::string_view, std::string_view>> path_vars_{};
ConstantEntry *constant_cache_ = nullptr;    // exchange pool,开放寻址
std::size_t constant_cache_capacity_ = 0;    // 2 的幂,0=未分配,初始 8
std::size_t constant_cache_size_ = 0;        // 负载 > 3/4 时倍增重散列(旧数组留给池,不释放)
```

- 探测:线性探查 `slot = hash & (cap-1)`,逐槽 `hash` 比较 + `name` memcmp(碰撞校验,构造性正确,
  与 `find()` 全链探测同标准)。
- `name` 借用安全性:条目只能由正在运行的脚本的 `constant_fn` 写入;契约(见 D9,注释已落在
  `ConstantEntry` / `resolve_constant` / `ConstPackage` 三处)= 脚本及其包必须活过它执行所针对的
  每一个 ctx。脚本自身编译产物(`FuncConst.userdata` = Ref*)本就要求同一契约,缓存未引入新前提。

## 4. resolve_constant 流程

```
probe cache → hit: return success(value)
miss:
  value = resolve_from_request(type, name)      // 无任何 GC 交互
  insert {hash, name, value}(可能先扩容;OOM → abort(OutOfMemory))
  return success(value)
```

`resolve_from_request` 逐类型(全部用 `const_name_equal` 对请求数据做规范化比较):

- **Header**:遍历 `exchange_.request_headers()`;`const_name_equal(name, field.lowcase_view())`
  命中即返回 `field.value_view()`(借用,D2 首个胜)。
- **Query**:`form_decode_query` 流式回调;每次命中 `copy_to_pool` 拷贝解码值并**覆盖**暂存
  (末个胜);结束后返回暂存(或无命中 null)。解码错误维持宽松:已解码部分可用。
- **Cookie**:逐 `cookie` 头 `decode_cookie_header`;首个命中返回(借用)。
- **Path**:线性扫 `path_vars_`;`const_name_equal(name, var.first)` 首个命中返回(借用)。
- **Context**:已删除,不可达。

## 5. 调用方迁移

| 位置 | 改动 |
|------|------|
| `apps/lite_nginx/src/proxy/ProxyHandler.cpp:101-107` | 删 `const_package` 判空 + `prepare_constants` + `bind_path_constants`;换 `ctx.set_path_vars(path_vars)`(该函数已收 `path_vars` 参数) |
| `apps/lite_nginx/src/runtime/ServerLauncher.cpp:166-177` | `run_script` 签名删 `const_package` 形参;`prepare/bind` 两行换 `ctx.set_path_vars(path_vars)`;调用点(:420)同步 |
| `apps/lite_nginx/src/logging/AccessLogger.cpp:112-124` | 删判空错误分支 + `prepare/bind`;换 `script_context.set_path_vars(context.path_vars)` |
| `apps/lite_nginx/src/runtime/RuntimeConfig.h` | **`const_package` 成员必须保留**:改完后没有任何代码调用包的方法,但它仍是编译脚本内 `FuncConst.userdata`(Ref*)指向内存的生命周期锚。在成员处加注释,防未来"看似无用"误删 |
| `tests/HttpScriptFuncsTest.cpp:356-358` | harness 删 prepare/bind,换 `set_path_vars(mc.vars)`;`CompiledRouteScript` 同样保留 `const_package` 持有权 |

## 6. ConstPackage.cpp 净变化

- Builder:`add_constant` 现算 hash 存入 Ref;`build()` 只做 seal(遍历 Ref 置 `sealed`)+
  移交 shared_ptr,**不再构建** `entries/buckets/entry_offsets/bucket_offsets`
  (Storage 缩到 `ref_pool` 一项)。
- 删除:`normalize_hash/normalized_equal` 内部副本(提公共)、`Entry`、`find()`、`entries()`、
  `identity()`、`bucket_count_for`、quadratic 探测表。
- 估计 −150 行左右,净简化。

### 6.1 noexcept 收口(2026-11-06 实施后补充,修复继承自旧代码的违约)

旧实现(及本次重写的初版)实际含抛点:`normalize_name` 返回 `std::string`(分配可抛)、
`unordered_map::emplace`、`vector::push_back`、`make_unique/make_shared` 控制块分配——
与全库 no-exceptions 规范相悖。现形态:

- **名字上限** `kMaxConstNameLength = 256`(公共常量,输入与规范化形等长):超限
  `add_constant` 返回 nullptr → 编译期 "constant not found"。上限使规范化可走
  `std::array<char, 256>` 栈缓冲,`normalize_name`(std::string)整函数删除。
- **去重改池化单链表线性扫**(Record 增 `next`,`unordered_map`/`vector` 删除):
  编译期每包几十条,O(n) 每次 add 完全可接受,换来零抛点。
- **shared_ptr 前置分配**:State ctor 即 `shared_ptr<ConstPackage>(new ConstPackage)`
  (裸 `new` 是因为私有 ctor 对 make_shared 的 std 内部不可见,踩过一次),`build()`
  变纯移动,真正 noexcept。
- **唯一抛点收口**:Builder ctor 内 try/catch bad_alloc(先例 = compile_template_string),
  OOM → state_ 为空 → add_constant 恒 nullptr、build 恒空包,均以编译错误面世。
- `add_constant`/`build`/`Builder()` 显式标 noexcept。

## 7. 测试计划

改造:
- `tests/ConstPackageTest.cpp`:去重/规范化用例保留(以 callable 同一性断言);
  `find/entries/index 布局/碰撞链` 用例随 API 删除。
- `tests/HttpScriptFuncsTest.cpp`:harness 迁移;现有请求视图/脚本行为用例全部保持绿
  (行为不变的回归网)。

新增(`ConstPackageTest` 或新 `ScriptExchangeCtxConstantTest`):
1. **跨包单 ctx(新能力 headline)**:两个不同 ConstPackage 编译两个脚本,同一 ctx 先后执行,
   各自常量均正确解析 —— 直接回归"配置热更中途换包"。
2. 重复键语义:同名多值 Header 首个胜 / Query 末个胜 / Cookie 首个胜。
3. miss → null($query/$header/$cookie/$path 各一)。
4. 记忆化:脚本内循环读同一常量,值稳定(间接验证缓存命中路径)。
5. `$path` 借用语义 + `set_path_vars` 在脚本前注入。
6. `$context` 改为编译期报错(原死路径,负例)。
7. lite_nginx 既有 proxy 模板 / access_log 集成测试不动,预期全绿。

## 8. 行为变化清单(评审对照用)

1. 常量物化 eager → lazy:未读常量零分配(仅分配画像变化)。
2. OOM 错误体:`500 SCRIPT_CONSTANTS` → `500 {"error":"OutOfMemory"}`(D5)。
3. 身份不匹配 abort(InvalidState → 500)消失,代之以跨包自然解析(D6)。
4. `$context.*` 从"恒 null 可编译"变为编译期报错(D4,全仓无使用者)。
5. 每请求常量相关工作从"每 ctx 全包预填 ×2~3"变为"每 ctx 实读集合一次解析"。

## 9. 实施与验证

- 单个原子提交(核心 API 删除即破坏 apps 编译,拆片须临时保留旧 API,不值得):
  `refactor(script): resolve request constants dynamically with per-request memoization`
  范围:src/http_script、include/fiber/http_script、apps/lite_nginx 三调用方、tests。
- 验证:`cmake --build build` → `ctest --test-dir build`(fiber_tests + lite_nginx_tests)
  → `./run_tidy.sh`(改头文件,触发 .o.d 闭包)→ `./format_code.sh` 收尾。

## 10. Follow-up(独立成片,不混入本次)

1. **单请求单 ctx + 单 heap**:ctx 构造在本设计后已近零成本,剩余重复仅
   query()/headers()/cookies() 视图按阶段重建与 GcHeap 构造。阻碍点:
   `AccessLogEvalContext` 向下转型(`AccessLogScriptExtension.cpp:119`)携带
   AccessLogScriptData,需改为数据旁路而非子类化。
2. 若引入请求态中途改写(URI rewrite 等),按 D8 加显式缓存失效钩子。
