# script 模块 noexcept 违约审计与修复

- 日期：2026-11-06
- 状态：**18 处全部修复完成**（2026-11-06，见 §8 进度表；checker 定向重跑归零）
- 范围：`src/script/`（重点 `src/script/std/` 宿主函数层）
- 方法：clang-tidy `bugprone-exception-escape` 定向重跑（该检查全库禁用，见
  `.clang-tidy` 与 `feature/clang-tidy-rollout.md` §2.1）+ 逐文件读码补漏

## 1. 问题定义

`src/script/std/` 的一批宿主函数声明为 `noexcept`，但函数体内使用抛式分配
（`std::string` 的 `reserve/resize/append/push_back`，直接或经 util 层间接）。
内存压力下 `bad_alloc` 逃出 noexcept 边界 → `std::terminate` → **整个网关进程死亡**。

## 2. 为什么这是真缺陷（而非全库政策可接受的 terminate）

全库政策（`.clang-tidy` 对 `bugprone-exception-escape` 的禁用理由）是：
"bad_alloc under noexcept boundaries is fatal by house policy"——对 common/net/tls
成立。但 script 模块**自身的契约**是 OOM 优雅中止：

- `BinaryFuncs.cpp` / `JsonFuncs.cpp` 的 `make_string_result` / `make_binary_result`：
  "mapping make_string's OOM (returns undefined) to a graceful abort"
- `StringsFuncs.cpp` `repeat_fn` 的 16 MiB cap 注释：
  "to stay clear of std::string's throwing reserve under OOM"（作者已在刻意规避，
  但 cap 只降概率不消除抛点）
- `GcHeap` 全链路非抛式（`BufPool::alloc` 返回 nullptr），`run/` 层的
  `gc_new_string_wtf8_uninit` 两段式是既定 OOM 安全惯用法

即：同一函数里，GC 侧分配失败会正确映射为 `AbiResult::abort(OutOfMemory)`，
而 C++ 暂存分配失败却 terminate。lite_nginx 按请求执行用户脚本，一个脚本
请求不该有能力杀死进程。

## 3. 触发机制

```
noexcept 宿主函数
  └─ std::string 暂存（reserve/append/...）
       └─ bad_alloc ──逃出 noexcept 边界──▶ std::terminate（进程死亡）
```

注意内层 noexcept 边界会在异常到达任何外层处理前就地 terminate（见 §4-C）。

## 4. 盘点清单（18 处）

行号为审计时（master e06e4ab0 后）原始行号。**ID 供逐项讨论引用。**

### A. 精确尺寸可预计算的转码型（Huffman 式两段式的候选，见 §5.1）

| ID | 位置 | 抛点 | 输出尺寸公式 |
|----|------|------|--------------|
| 1 | `BinaryFuncs.cpp:70` `base64_encode_fn` | `util::base64_encode` 返回 `std::string`（util 内部构造+push_back） | `4*⌈n/3⌉`，ASCII ⇒ utf16 同长 |
| 2 | `BinaryFuncs.cpp:86` `base64_decode_fn` | `std::string out` 交 `base64_decode` 填充 | `3*n/4 - pad`，结构校验可先行 |
| 3 | `BinaryFuncs.cpp:104` `hex_fn` | `hex_str.resize(len*2)` | `2n` |
| 4 | `BinaryFuncs.cpp:122` `from_hex_fn` | `out.resize(n/2)` | `n/2`（合法性校验可与写入同趟） |
| 5 | `StringsFuncs.cpp:222` `to_lower_fn` | `out.reserve(n)` + `push_back` | `n` 字节；utf16 需 `utf16_len(text)` 另计 |
| 6 | `StringsFuncs.cpp:235` `to_upper_fn` | 同上 | 同上 |
| 7 | `StringsFuncs.cpp:630` `repeat_fn` | `out.reserve(i*len)`（已有 16MiB cap） | `i*len` 字节、`i*u16len`；cap 保留 |

### B. 尺寸不可廉价预计算的暂存型（oom_guard 兜底，见 §5.2）

| ID | 位置 | 抛点 |
|----|------|------|
| 8 | `ArrayFuncs.cpp:93` `array_join_fn` | `delimiter`/`out` 两个 `std::string` 的 append（元素经 `append_as_text`） |
| 9 | `HashFuncs.cpp:32` `crc32_fn` | `node_as_text` 填充 `std::string` |
| 10 | `RandFuncs.cpp:91` `canary_fn` | 每 key 一个 `std::string`（`node_as_text`） |
| 11 | `StringsFuncs.cpp:717` `to_string1_fn` | `node_json_to_string` 填充 |
| 12 | `BinaryFuncs.cpp:149` `get_utf8_bytes_fn` | 同上 |
| 13 | `JsonFuncs.cpp:110` `stringify_fn` | `StringSink` 往 `std::string` 追加（经 `Generator`） |
| 14 | `UrlFuncs.cpp:139` `url_encode_component_fn` | `util::form_encode(input, out)` |
| 15 | `UrlFuncs.cpp:159` `url_decode_component_fn` | `util::form_decode` 返回 `IoResult<std::string>` |
| 16 | `UrlFuncs.cpp:182` `url_parse_query_fn` | `form_decode_query` 模板内部 `key_buf/val_buf` |
| 17 | `UrlFuncs.cpp:228` `url_build_query_fn` | `form_build_query` 的 `out` + `QueryPairSource` 内 `key_buf` |

### C. 内层 noexcept 辅助体（异常到不了外层兜底，须去 noexcept）

| ID | 位置 | 说明 |
|----|------|------|
| 18a | `JsonFuncs.cpp:31` `StringSink::write` 的 `noexcept override` | 基类 `OutputSink::write`（`JsonEncode.h:16`）本无 noexcept，此处过度收紧；`out_.append` 抛出当场 terminate |
| 18b | `UrlFuncs.cpp:100` `QueryPairSource::operator()` 的 noexcept | 内部 `key_buf` 赋值 / `node_json_to_string` / `gc_string_to_utf8` 皆可抛；`form_build_query` 模板对 source 无 noexcept 要求 |

### 检查器覆盖情况

- A/B 共 18 处中，checker 报出 14 处；**读码补漏 4 处**：ID 1、14、15、17 中
  的 encode/decode_component 与 build_query 主体、base64_encode（checker 对
  直接调用非 noexcept util 函数的路径存在漏报，清单不可只依赖 checker）。
- checker 命令（定向重跑，勿全局启用）：
  ```bash
  grep -oE '"file": *"[^"]*"' build/compile_commands.json | sed 's/^"file": *"//; s/"$//' \
    | grep '/src/script/.*\.cpp$' | grep -v http_script \
    | xargs -P 12 -n 1 clang-tidy-20 -p build -quiet \
        --checks='-*,bugprone-exception-escape' --warnings-as-errors='' 2>/dev/null \
    | grep exception-escape
  ```

## 5. 修复方向

### 5.1 A 类：Huffman 式两段式（util 层加 `_size`/`_into`，GC 侧直写）

**已定谳：ID 1/2（base64 encode/decode）先行**，与 HPACK Huffman 同型——
`src/http/Huffman.h` 的范式：

- `hpack_huffman_encoded_length(src,len) -> size_t noexcept`（纯计数不写）
- `hpack_huffman_encode_exact(src,len,dst) noexcept`（写入调用方精确尺寸缓冲）
- 全系 `noexcept`、`[[nodiscard]]`、`(src,len,dst,cap)` 纯函数，编解码器内零分配零 `std::string`
- 调用方（`Http2HpackEncoder.cpp:287,311`）：先算长→保证缓冲→`_exact` 直写→校验 written

base64 对应改造（**ID 1/2 已实施**，实施记录见本节末）：

1. `include/fiber/common/util/Base64.h` 新增四原语（实施时定谳：**旧
   `std::string` API 直接删除**，不再保留——全部调用方迁移为
   resize(encoded_size) + `_into` 两步，非 noexcept 调用方自己持缓冲）：
   ```cpp
   // 精确输出长度（不写、不校验字母表）
   [[nodiscard]] std::size_t base64_encoded_size(std::size_t len) noexcept;
   // 结构校验（长度%4、pad 位置/数量）+ 精确解码长度；ok=false 即 RangeError 依据
   [[nodiscard]] std::size_t base64_decoded_size(std::string_view in, bool &ok) noexcept;
   // 直写精确尺寸缓冲；返回 false 仅当字母表非法（结构问题已在 decoded_size 排除）
   void base64_encode_into(const std::uint8_t *data, std::size_t len, char *dst) noexcept;
   bool base64_decode_into(std::string_view in, std::uint8_t *dst) noexcept;
   ```
   调用方迁移：TlsSignature、TlsCertificate、HttpWebSocketProxy ×2、JsonEncode、
   tests/TlsCertificateTest、apps/lite_nginx/tests/LiteNginxRuntimeTest。Base64.h
   不再传递包含 `<string>`，TlsSignature/TlsCertificate 补显式 include。
2. 脚本侧 `BinaryFuncs.cpp`：
   - encode：`base64_encoded_size` → `gc_new_string_wtf8_uninit(heap, n, n, true)`
     （base64 输出纯 ASCII，well_formed=true）→ `base64_encode_into` 直写
     `gc_string_wtf8_data(str)` → `gc_detail::gc_string_intern_final` → 包装为
     GcString 值。OOM（uninit 返回 null）→ `AbiResult::abort(OutOfMemory)`。
     零暂存、零拷贝。
   - decode：`base64_decoded_size`（false → RangeError）→ 二进制两段式。
     `GcBinary.data` 是独立 `gc_alloc_extra` 分配（`GcArray.cpp:18` 现为拷贝式
     `gc_new_binary(heap,data,len)`），需新增
     `gc_new_binary_uninit(heap, len) -> GcBinary*`（镜像
     `gc_new_string_wtf8_uninit`，失败同样回滚 header）→ `base64_decode_into`。
   - GC 注意：沿用 `Binaries.cpp:266-285` 的范本——`LocalMark` 持根 +
     `NoGcScope` + uninit 分配失败路径。
3. utf16_len：base64/hex 输出为 ASCII，`utf16_len == byte_len`，无需另算。

**ID 1/2 实施记录（2026-11-06）**：上述 1/2 全部落地，`gc_new_binary_uninit`
按镜像方案实现；测试 `tests/Base64Test.cpp`（RFC 4648 §4 KAT + 0..70 全长度
round-trip + 结构/字母表拒绝矩阵）。同趟性能改造（用户要求）：decode 由
`decode_digit` 分支链改为 **256 项静态反查表**（无效值 0xFF，四 quad 值 OR 后
单分支测 bit7 合并校验，pad 尾 quad 剥离），encode 主循环 ×4 展开（原本已是
64 项表查）。/tmp A/B 微基准（clang++ -O2，ABAB 交错，best-of-6，输出逐字节
对拍相等）：decode **6.3×**（≥64KB：0.11→0.70 GB/s，随机数据下旧分支链误预测
严重；1KB 也有 1.6~1.8×），encode +2~7%（表查本就在，仅省循环分支）。
2554 fiber_tests + 109 lite_nginx_tests 绿。**ID 3/4（hex/fromHex）同日已修**：
BinaryFuncs.cpp 内纯 GC 两段式迁移（`hex_encode` 已是 noexcept 直写，digit 循环
单趟填 `gc_new_binary_uninit` 的 data），语义/报错种类零变化；checker 定向重跑
BinaryFuncs 仅剩 ID 12（get_utf8_bytes_fn），2554 绿。**ID 5/6（toLower/toUpper）
同日已修**：合体 `ascii_case_flip` 共用体（ASCII 翻转保长），utf16_len/well_formed
取自输入元数据（堆串 header / 借用串 utf8_scan，借用串扫描失败视为不可达路径
abort，与旧路径 make_string 失败行为一致）；checker StringsFuncs 仅剩 ID 7/11，
2555 绿。**ID 7（repeat）同日已修**：`string_utf16_meta` 抽为共用 helper，
填充 memcpy 倍增，全部早退/窄化/cap 语义不变；checker StringsFuncs 仅剩 ID 11，
2556 绿，**A 类（ID 1-7）全清**。

**B/C 类（ID 8-17 + 18a/18b）2026-11-06 同日全清**：`include/fiber/script/std/OomGuard.h`
落地（bad_alloc-only，见 §5.2 定谳）；10 处函数体原样移入 `oom_guard([&]{...})`
（ArrayFuncs/HashFuncs/RandFuncs/StringsFuncs/BinaryFuncs/JsonFuncs/UrlFuncs×4），
18a/18b 先行去 noexcept（guard 接得到异常的前提）。函数体逐字未动（仅缩进），
行为零变化。checker 定向重跑**归零**（guard 模板带 NOLINT 豁免锚点）；
2556 fiber_tests + 109 lite_nginx_tests 绿。**tidy 收口（2026-11-06）**：首轮
22 findings（Base64.cpp 3：loop-convert/math-parentheses/nested-ternary；
Base64Test 19：designated-initializers/qualified-auto）手工修复后
`./run_tidy.sh` exit 0；重建复验 2556+109 绿。**审计 18 处全部完成并收口，
仅剩提交。**

其余 A 类（ID 3-7）同法：hex/from_hex 直接套；to_lower/to_upper/repeat 需加算
`utf16_len`（`utf8_scan` 已有），repeat 保留 16MiB cap。

### 5.2 B 类：oom_guard 兜底

新增 `include/fiber/script/std/OomGuard.h`（模板，先例：`ScriptCompiler.cpp:37-61`
`compile_template_string` noexcept 内 catch `bad_alloc` → ParseError，handler
自身不分配故 noexcept 成立）：

```cpp
template<typename Body>
AbiResult oom_guard(Body &&body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc &) {
        return AbiResult::abort(ScriptAbortReason::OutOfMemory);
    }
}
```

- 各函数签名不动，函数体原样移入 `oom_guard([&] { ... })`，行为零变化
- `catch (...)` vs 仅 `catch (const std::bad_alloc &)`：**已定谳仅 bad_alloc**
  （2026-11-06，用户批准）——这些函数体的抛点全部是 `std::string` 分配
  （`length_error` 在 64 位下物理不可达），其他异常逃逸本身是 bug，terminate
  更诚实，与 house policy 哲学一致。guard 模板因此带
  `NOLINT(bugprone-exception-escape)`（理由：非 bad_alloc 按设计逃逸），
  checker 对该锚点豁免
- 深度优化（ScratchBuf/sink 化，消除暂存拷贝）列为后续可选项，不阻塞本审计
- **C 类为 B 类前置**：ID 13/17 的 guard 要能接到异常，18a/18b 的内层
  noexcept 必须先去掉（bad_alloc 在内层边界会就地 terminate）

### 5.3 C 类：去掉内层 noexcept

- ID 18a：`StringSink::write` 去 `noexcept`（基类本无，属过度收紧）
- ID 18b：`QueryPairSource::operator()` 去 noexcept，异常传至 ID 17 的 guard

## 6. 范围外关联项（不在本审计修，记录在案）

- `http_script/ScriptExchangeCtx.cpp` 同款 `StringSink`（`write_json`/`write_error_json` 路径）——**已修
  （2026-11-06，用户定谳 IoBufChain 方案取代 try/catch 兜底）**：StringSink 删除，新增 ChainSink
  （JSON 生成器直写 IoBufChain 传输缓冲，节点容量 16KiB−控制块命中节点池，`write()==false` 即
  OutputSink 既有 OOM 通道，sink 自持 `oom_` 标志区分 Generator 的 ErrorState）；`send_final_with_body`
  加 chain 重载（`mark_complete()` 即链路径 end 标记，HttpExchange::write_all 读取）；OOM→`IoErr::NoMem`
  、值形编码错保持 `Invalid`；省一次全响应体暂存拷贝。回归测试 ×2（>16KiB 单大 token / 2100 小 token
  渐进填充跨节点，逐字节对拍）。2886 全绿。
- **http_script 既有项新发现（2026-11-06，首次把 checker 扫到 src/http_script，14 处，全部先于本次
  改动存在，待逐项讨论）**：`HttpClientFuncs.cpp:312` 第三个 StringSink 孪生（http 客户端请求体
  JSON 编码，`build_request_body`）+ 同文件 `object_pairs_to_form`/`apply_options_headers` ×2/
  `url_field_error`；`ResponseFuncs.cpp` `set_header_fn`/`add_header_fn`/`add_cookie_fn`；
  `ScriptExchangeCtx.cpp` `query`/`resolve_from_request`（query decode 的 std::string 拷贝）；
  `HttpTarget.cpp:83` `parse`；`CookieCodec.h:48` `decode_cookie_header` 模板（经 ScriptExchangeCtx
  两处 lambda 实例化报出）
- `HttpWebSocketProxy.cpp` `build_expected_websocket_accept`（:42）与
  `prepare_websocket_handshake`（:132）noexcept 但 `std::string::resize` 抛式
  （迁移 base64 时发现）——**已修（2026-11-06，随审计收尾追加）**：两产物均
  定长（base64(RAND 16B)=24、base64(SHA1 20B)=28，皆超 SSO 故原为堆分配），
  `WebSocketHandshake` 字段改定长数组 + `key_generated`/`accept_ready` 就绪位
  + `key_view()`/`accept_view()`（未就绪返回空 view，与旧空串语义逐位一致），
  两函数变零分配，`static_assert` 锁长度；调用方（ProxyHandler designated-init、
  HttpClientFuncs 默认构造）零改动，checker 定向该 TU 归零
- util 层 `std::string` 系 API（UrlForm 等；Base64 的 std::string 版已随 ID 1/2 删除）
  对非 noexcept 调用方按 house policy 维持现状
- `.clang-tidy` 的 `bugprone-exception-escape` 全局禁用维持不变（本审计用定向重跑做验收）
- 性能向：A 类两段式顺带消除暂存拷贝（一次 GC 分配直写），属增益非目标
  （base64 的查表提速为 ID 1/2 同趟的顺带实施，已记录于 §5.1 实施记录）

## 7. 验收

- 每片合入后：§4 的 checker 定向重跑归零（或仅剩已定谳不改项）
- 全量 `ctest --test-dir build` 绿；新增函数级测试（base64 已有：RFC 4648 §4
  KAT + round-trip + 拒绝矩阵；后续 hex 等同规格；脚本层 base64/hex
  round-trip、OOM→abort 映射）
- `./format_code.sh` + `./run_tidy.sh` 清零（tidy 按用户要求最后统一跑）

## 8. 进度

| ID | 修复方向 | 状态 |
|----|----------|------|
| 1 | 两段式（先行，用户定谳参考 Huffman）+ 查表提速 | **已修**（2026-11-06，含旧 API 删除与 6 调用方迁移，见 §5.1 实施记录） |
| 2 | 两段式（先行，同上） | **已修**（同上，decode 查表 6.3×） |
| 3 | 两段式 | **已修**（2026-11-06；`hex_encode` 本就是 noexcept 直写原语，无 util 层改动，纯 GC 两段式迁移） |
| 4 | 两段式 | **已修**（同上；`gc_new_binary_uninit` 复用 base64 片；校验/填充单趟语义不变） |
| 5 | 两段式（+utf16 计数） | **已修**（2026-11-06；与 6 合体为 `ascii_case_flip`：翻转保字节长与 UTF-16 单元数，输出复用输入 utf16_len/well_formed——堆串 header 免费取、借用串一趟 `utf8_scan`；新增 substring 按 UTF-16 单元切片的元数据保持测试） |
| 6 | 两段式（+utf16 计数） | **已修**（同上） |
| 7 | 两段式（+utf16 计数，留 16MiB cap） | **已修**（2026-11-06；`string_utf16_meta` 抽为共用 helper（5/6 同款双源取元数据），填充改 memcpy 倍增 O(log i)；null/empty/i==1 早退、int32 窄化/饱和、cap→abort 语义全部不变；A 类至此全清） |
| 8 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 9 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 10 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 11 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 12 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 13 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 14 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 15 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 16 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 17 | oom_guard | **已修**（2026-11-06，oom_guard bad_alloc-only） |
| 18a | 去 noexcept | **已修**（2026-11-06；ID 13 guard 的前置） |
| 18b | 去 noexcept | **已修**（2026-11-06；ID 17 guard 的前置） |
