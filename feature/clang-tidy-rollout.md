# clang-tidy 引入与分批清零

记录日期:2026-10-03;工具:clang-tidy-20(20.1.8,与工具链 clang-20 同版本)。
关闭 `docs/project-review-2026-10.zh-CN.md` 工程缺口 #2(无 clang-tidy)。

**状态:基础设施已落地(cd18d5d1);全量基线扫描完成,分诊结论见 §2;
批次 1(src + example)修复进行中。**

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
  - 退出码:0 干净 / 1 有告警 / 2 环境错误(CI-ready)。
- 不做 `CMAKE_CXX_CLANG_TIDY` 内嵌(拖慢每次构建);v1 不含 `WarningsAsErrors`
  (批次 1 清零后再启用)。

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


## 3. 批次划分

- 批次 1:src/ + include/(经 src TU 覆盖)+ example/(14 TU,补公头覆盖缺口)
- 批次 2:apps/(nacos/cat/prometheus/lite_nginx)
- 批次 3:tests/(GoogleTest 宏噪声,开工首步先做局部基线;宁可行级
  NOLINT 不禁检查)
- fuzz/:须用 build-fuzz 的 db(FIBER_BUILD_FUZZERS 树才有 fuzz TU)

## 4. NOLINT 清单

(修复期追加,每条带理由。)

## 5. Backlog(tier-2 候选)

- misc-include-cleaner(相对包含风格下海量 churn)
- readability-identifier-naming(需独立命名规范项目)
- cppcoreguidelines 精选(pro-bounds/pro-type-ownership 与性能优先设计冲突,只挑不冲突项)
- CI 接线(评审缺口 #1,另案)
- 编译器告警旗标(-Wall/-Wextra)为独立后续事项
