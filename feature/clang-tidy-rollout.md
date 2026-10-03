# clang-tidy 引入与分批清零

记录日期:2026-10-03;工具:clang-tidy-20(20.1.8,与工具链 clang-20 同版本)。
关闭 `docs/project-review-2026-10.zh-CN.md` 工程缺口 #2(无 clang-tidy)。

**状态:批次 1(src + include + example)已完成清零并提交(13 个模块提交,
见 §3.1;compression/log 为终扫补入的两模块);收官终扫 `./run_tidy.sh src
example`(260 TU)0 发现 0 前端错误;根 `.clang-tidy` 已启用
`WarningsAsErrors: '*'`,批次 2/3 目录经嵌套 `.clang-tidy` 暂缓(见 §6)。**
批次 2/3 见 §7 backlog。

## 1. 基础设施

- `CMakeLists.txt`:`project()` 后一行 `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`,
  所有构建树导出 compile_commands.json。
- `.clang-tidy`:tier-1 检查集(bugprone/performance/modernize/readability 通配
  减禁用清单 + misc 精选 + clang-analyzer 通配减预禁用)。
  `HeaderFilterRegex: (^|/)(src|include)/` 限定项目头文件,
  `ExcludeHeaderFilterRegex` 排除 temp/_deps、系统头(/usr)、构建树生成头。
- `run_tidy.sh`(镜像 `format_code.sh` 惯例):
  - 版本探测 clang-tidy-20→17→裸 clang-tidy,**主版本 <17 拒跑**;
  - 默认只查改动 TU;改动的头文件按文件名扇出到包含它的 TU(>50 提示);
  - 位置参数支持单文件/目录(替换改动集),`-a` 全量,`-f` 修复+格式化+复验;
  - compile-db 成员预过滤(新文件/陈旧 GLOB 给可见提示);
  - db 编译器为 g++ 时退出 2(SelectToolchain 自动选 clang,重配即好);
  - 优先 run-clang-tidy(-fix 时跨 TU 同头文件修复去重);缺则 xargs 回退,
    回退的 -f 逐文件串行(不做并发 -fix,避免同头文件并发写丢失);
  - 排除 `**/generated/**`(其目录级 `Checks: '-*'` 会让 tidy 硬错);
  - 退出码:0 干净 / 1 有告警 / 2 环境错误(CI-ready)。
- 不做 `CMAKE_CXX_CLANG_TIDY` 内嵌(拖慢每次构建)。

基线环境:build/ 树(clang++-20 + libc++,`-stdlib=libc++ -isystem
/usr/lib/llvm-20/include/c++/v1`),545 个项目 TU(src 248 / apps 88 / tests 183 /
example 14 / fuzz 12),-j8。

## 2. 基线分诊(2026-10-03)

全量基线:529 TU、41,348 条告警(含头文件随 TU 重复计数),零前端错误。
按 `文件:行:检查` 去重后 **6,706 条**;按目录:include/ 2174、src/ 1156、
tests/ 2543、apps/ 693、example/ 140、fuzz 0(fuzz TU 仅在 build-fuzz db)。

### 2.1 分诊决策(禁用清单)

| 检查 | 去重计数 | 决策 | 理由 |
|---|---|---|---|
| readability-redundant-member-init | 1360(批次1 1198) | 禁用 | 打的全是"ctor 初始化列表重复 NSDMI"的显式初始化(IntrusiveList/EventLoop/IoBufChain 核心);与"构造边界建立不变量、成员显式初始化"的库规直接冲突,自动删除有误判致未初始化成员的风险 |
| performance-enum-size | 106 | 禁用 | 枚举宽度是 wire/ABI 刻意选择(IoErr:u16、TLS suite 表),自动缩宽破坏协议布局 |
| modernize-use-std-print | 261 | 禁用(tier-2) | 全是冷路径 fprintf(Assert.h 等)的化妆性替换,format 串转换有风险,churn 大 |
| clang-analyzer-cplusplus.NewDeleteLeaks | 60 | 禁用 | 抽样全部落在 libc++ unique_ptr `__ptr_` 内部字段(analyzer 建模缺陷),非真泄露 |
| clang-analyzer-security.PointerSub | 15 | 禁用 | 侵入式条目 `handle_offset = (char*)&entry - (char*)&handle` 是刻意 container_of 偏移惯用法 |
| performance-no-int-to-ptr | 11 | 禁用 | 7 条在生成代码 Http1HeaderLineParser,其余为刻意 tagged-pointer 往返 |
| bugprone-exception-escape | 127 | 禁用 | 100% 策略性误报(60+ 抽样):noexcept 边界下的 bad_alloc 按库规即致命;expected/optional 有守卫的 `::value()` 非路径敏感;libc++ 经 `__throw_bad_expected_access` 抛出,`IgnoredExceptions` 四种拼写均无法匹配(已逐一验证)。检查获得路径敏感性或能匹配 libc++ 抛出助手时重开;存量站点 NOLINT 保留作文档 |
| src/http/generated/(目录) | 34 | 嵌套 .clang-tidy `Checks: '-*'` | TS 生成器产物(scripts/http/generate_header_line_parser.ts);修生成器另案,不修产物 |

analyzer 检查点:core.* 家族抽样(NullDereference 7、deadcode.DeadStores 12、
uninitialized.Branch 3)未见侵入式结构系统性误报,**保留 clang-analyzer 通配**
(仅禁上述两项);后续批次若 FP 率过半再降级为显式清单。

### 2.2 批次 1 存量(定稿配置后)

src/ + include/ + example/ 去重 **1,838 条**,大头:
designated-initializers 420、uppercase-literal-suffix 174、equals-delete 136、
exception-escape 127(无自动修,人工)、macro-parentheses 102、named-parameter 81、
implicit-widening-of-multiplication-result 80、simplify-boolean-expr 75、
return-braced-init-list 71、qualified-auto 62、unused-return-value 55(人工)、
make-member-function-const 52、move-const-arg 50、math-missing-parentheses 44…
其余 50+ 检查均 <40 条。**协程相关:use-after-move 6、
unnecessary-value-param 5(逐条人工,禁按提示改引用)。**

### 2.3 CheckOptions 调参

- `readability-simplify-boolean-expr.SimplifyDeMorgan: false` — 取反改写与
  库内"正形复合 FIBER_ASSERT 不变量"惯用法相斥,检查其余facet(冗余 !!、
  与 bool 字面量比较等)保持开启。
- `bugprone-unused-return-value.AllowCastToVoid: true` — `(void)expr;` 是
  abort/close 尽力而为路径的库内意图标记;与 cert-err33-c 不同,此检查
  默认不认 (void) 转换。

## 3. 批次划分

### 3.1 批次 1(已完成,每模块独立提交,均过"构建零错+tidy 零告警+ctest 全绿")

| 模块 | 提交 |
|---|---|
| common | 1f820808 |
| event | c95661c9 |
| async | ec280163 |
| net | a0c219c4 |
| tls | 685b9009 |
| http | 7d803fb9 |
| quic | c513c1af |
| dns | a41ba7c2 |
| script | 975bd180 |
| http_script | c949286f |
| example(含经 nacos_demo TU 带入的 apps/nacos 三个 option 头) | 65049065 |
| compression(计划模块序漏列,全量终扫补入;zlib 移植惯用法 3 类 NOLINT) | bd41add2 |
| log(同上补入;含 LoggerManager 同长名次序非确定性真修) | 24259065 |

compression/log 两模块原计划模块序遗漏,批次 1"收官终扫"暴露 154 条后才补做;
教训:计划里的模块清单要对照 `git ls-files src/ | cut -d/ -f2 | sort -u` 复核。

### 3.2 批次 2/3(待做)

- 批次 2:apps/(nacos/cat/prometheus/lite_nginx)
- 批次 3:tests/(GoogleTest 宏噪声,开工首步先做局部基线;宁可行级
  NOLINT 不禁检查)
- fuzz/:须用 build-fuzz 的 db(FIBER_BUILD_FUZZERS 树才有 fuzz TU)

## 4. NOLINT 清单(批次 1 后共 85 行守卫,按检查名)

每处带理由注释(NOLINTNEXTLINE 上方;区域用 NOLINTBEGIN/END;计数按守卫行,
BEGIN/END 各计一行;行守卫 ≤120 列。核对命令:
`grep -rn 'NOLINT' src include example | wc -l`)。

| 检查 | 行数 | 典型理由 |
|---|---|---|
| bugprone-unchecked-optional-access | 13 | 构造边界不变量解引用(如 ConnectionGroupKey::make 对已知合法输入不可能失败);生产路径已守卫,analyzer 不跨函数(log 批的 3 处已真修:形参改传 engaged 引用) |
| bugprone-exception-escape | 9 | 检查已整体禁用前的存量守卫,保留作文档(见 §2.1) |
| readability-simplify-boolean-expr | 8 | DeMorgan 关闭后剩余的守卫(与正形断言冲突的个别形态) |
| clang-analyzer-deadcode.DeadStores | 8 | 防御性存储(错误码兜底、边界默认值);compression 的 2 处为 zlib 移植丢弃末次存储惯用法(insert_string 尾环) |
| bugprone-use-after-move | 6 | IoBuf/链 moved-from 为合法空态复用(re-emplace、swap-and-clear) |
| clang-analyzer-unix.Stream | 4 | fd 生命周期由 RAII 包装保证,analyzer 不跨构造 |
| bugprone-suspicious-stringview-data-usage | 4 | data() 作稳定指针传递(非 C 串),长度另行成对传 |
| misc-redundant-expression | 3 | 宏展开重复(zlib `<=5` 冗余守卫逐字保留等) |
| bugprone-unused-return-value | 3 | 无 (void) 形态的尽力而为调用(自动修不覆盖) |
| bugprone-string-constructor | 3 | `string_view("", 0)` 非空规范化(make_native_string 需可解引用基址) |
| bugprone-inc-dec-in-conditions | 3 | zlib 展开的 8 路 ++scan/++match 比较(1 对 BEGIN/END)与 best_len 前置探测,逐字保留上游 |
| clang-analyzer-unix.StdCLibraryFunctions | 2 | dirfd() 对活 DIR* 不会失败,analyzer 值域模型保守 |
| bugprone-nondeterministic-pointer-iteration-order | 2 | 比较子对指针集是严格全序(注册表 logger 名唯一);另一处已改 stable_sort 真修后保留守卫 |
| clang-analyzer-optin.core.EnumCastOutOfRange | 2 | HttpVersion 无固定底层类型,写值方为受控序列化点 |
| clang-analyzer-core.uninitialized.Branch | 2 | 协程 promise 帧构造被 analyzer 误建模(NSDMI 已值初始化) |
| clang-analyzer-core.StackAddressEscape | 2 | RAII 观察者配对释放,analyzer 不跨函数 |
| clang-analyzer-core.NullDereference | 2 | 侵入式结构重链接丢对象同一性 |
| clang-analyzer-core.NonNullParamChecker | 2 | 同上 |
| bugprone-not-null-terminated-result | 2 | HKDF info 结构对 label/context 长度前缀,非 C 串语义 |
| bugprone-macro-parentheses | 2 | 宏参数在展开点已有括号保护 |
| clang-analyzer-core.CallAndMessage | 1 | 跨函数 RAII 契约 |
| bugprone-unhandled-self-assignment | 1 | 类型不可拷贝 |
| bugprone-branch-clone | 1 | 宏展开产生的相似分支 |

## 5. 分诊守则(批次 2/3 沿用)

- **协程按值参数绝不"修"成引用**(unnecessary-value-param):跨 co_await 挂起
  存于协程帧,改 const& 反而悬空 —— 逐点 NOLINT,检查保持开启。
- 条件消费型右值 API 是 use-after-move 误报主族:`try_merge_adjacent(IoBuf&&)`
  仅成功时消费、`try_attach_local_stream(Lease&&)` Busy 路径先返回不碰 lease,
  实现核验后才判 FP。
- ctor 不变量 optional 解引用 → NOLINT;数据面防御性存储 → 保留 + DeadStores NOLINT;
  真缺陷一律真修 + 回归测试(见 §8)。
- 头文件诊断随包含它的 TU 走 —— 批次 1 期间 apps/nacos 三个头经 nacos_demo.cpp
  被带入修复即此机制(HeaderFilterRegex 匹配任意 `include/` 段)。

## 6. WarningsAsErrors 与嵌套覆盖

- 根 `.clang-tidy` 已加 `WarningsAsErrors: '*'`(批次 1 清零后的守门)。
- `apps/`、`tests/`、`fuzz/` 各置嵌套 `.clang-tidy`(`InheritParentConfig: true`
  + `WarningsAsErrors: '-*'`)使批次 2/3 区域暂不致命;**注意 InheritParentConfig
  对标量是追加不是覆盖**:子配置 `WarningsAsErrors: ''` 合成 `'*,'` 仍然全致命,
  必须用 `'-*'`(合成 `'*,-*'`,glob 后者胜)。批次落地清零后删除对应嵌套文件。
- fuzz 的 TU 只存在于 build-fuzz db,检查须 `-p build-fuzz`。

## 7. v20 自动修复损伤模式(-f 后必扫)

每轮 `-f` 后、构建门之前,用三件套 grep 扫描(经验全部来自本分支实伤):

1. **共享头多 TU 重复应用**:`.reason =.reason =.reason =` 连写设计器、
   `const const const noexcept`、`static_cast<T>(static_cast<T>(...))`
   (run-clang-tidy 的去重不覆盖"同一头文件经多条相对路径包含"场景;
   ScriptResult.h 曾三重叠加)。
2. **modernize-use-integer-sign-comparison 吞/吐括号**:`std::cmp_greater(...))) {`
   多右括号,且自动修不补 `<utility>` 包含(DnsResolverConfig 实伤;嵌套
   sizeof 的 `)))` 是合法形态,以编译器 `extraneous ')'` 为准反查)。
   **根因(v20 检查语义):显式 `static_cast<int>(unsigned常量)` 后仍告警** —
   检查要求改成 `std::cmp_*` 并剥掉转型,fixit 因此动到 for 头部结构才脆裂。
   手修策略:计数器仅作索引 → 计数器改 unsigned(连转型一起删);计数器必须
   保持 int(与 int 变量比较)→ `std::cmp_less/less_equal` + 补 `<utility>`。
   compression 批 17 处全走手修,无一交给 -f。
3. **窄化转换修复器在常量初始化处产出 `static_cast<const T>`**(带 cv 限定,
   合法但怪;统一手改为去 const 形态)。
4. **NOLINT 行被 clang-format 折行**断守卫 —— 守卫行保持 ≤120 列。
5. **modernize-return-braced-init-list 可能不给 fixit**(无法证明无重载/列表
   构造歧义时只告警不修)→ FormattedLogRecord.h 两处手改 `return {...}`。

**陈旧二进制陷阱**:autofix 损伤致编译错时,ctest 仍对旧二进制全绿。每道门
必须含 `cmake --build … | grep -E "error|warning"`(空才算过),且 tidy 日志
`: error:` 计数为 0。

## 8. 真缺陷(批次 1 战果)

| 缺陷 | 位置 | 内容 |
|---|---|---|
| HPACK 整数第 5 续位字节 UB | Http2HpackDecoder | 移位达 ≥35 仍继续;改为 `integer_shift_ >= 28U` 即拒 + 回归测试 RejectsFifthIntegerContinuationByte |
| HKDF info 栈缓冲下溢 | QuicCrypto::hkdf_expand_label | 栈房间按 32 字节预留但守卫放行 label≤249/context≤255;前移上限守卫 + 定长房间 |
| strcpy→memcpy | QuicCrypto | 长度前缀区用 strcpy 语义错误 |
| future::get() 二次调用 | example/tls_ab_bench | writer/server 失败路径打印错误时对已消费 future 再 get()(UB);提升为单次 get() |
| 同长 logger 名解析次序非确定 | LoggerManager | matching_rules 按 name.size() 排序,同名长并列时以堆序(地址序)决出先后,last-wins 级别解析随之不定;改 std::stable_sort 保配置声明序 |

## 9. stdlib 交叉验证(计划步骤 9:本机不可执行)

原计划建 libstdc++ 树复跑 src 批比对零 delta。本机系统 libstdc++ 为 GCC 10
(无 `<expected>`),未装 GCC 13+,`CompilerChecks.cmake` 依规拦截(报错信息
明确)。残留风险低:stdlib 头已被 ExcludeHeaderFilterRegex 全部排除,诊断面
只剩项目代码与编译旗标。**待有 GCC 13+ 的机器补跑**:
`cmake -S . -B build-std -DFIBER_USE_LIBCXX=OFF -DFIBER_DEPS_DIR=$PWD/temp/_deps_std`
(须独立 deps 目录,勿混 temp/_deps),然后 `./run_tidy.sh -p build-std $(git ls-files 'src/*.cpp')`。

## 10. Backlog(tier-2 候选)

- misc-include-cleaner(相对包含风格下海量 churn)
- readability-identifier-naming(需独立命名规范项目)
- cppcoreguidelines 精选(pro-bounds/pro-type-ownership 与性能优先设计冲突,只挑不冲突项)
- CI 接线(评审缺口 #1,另案)
- 编译器告警旗标(-Wall/-Wextra)为独立后续事项
- tests/ 既有编译器级 nodiscard 告警(ClientHttp1ExchangeTest.cpp:1171/1966),
  属 -Wall 事项,非 tidy 范畴
