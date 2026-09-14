# zlib 1.3.2 压缩核心迁移方案

状态：设计，尚未实施。本文只确定迁移方案，不表示相关接口、构建目标或测试已经存在。

日期：2026-09-14。基线：直接采用官方 zlib 1.3.2 源码。

## 1. 目标与范围

将当前 HTTP gzip 所需的压缩核心迁入项目，形成使用项目内存、错误和缓冲基础设施的 C++23 实现。正常配置、构建和运行不下载、不查找、不链接外部 zlib；公开头文件不暴露 zlib 类型、宏或私有实现。

本次交付包含：

- gzip 流式编码，内部使用 raw DEFLATE 核心。
- 压缩等级 1～9，默认等级 1；固定 windowBits=15、memLevel=8、default strategy。
- stored、fixed Huffman、dynamic Huffman 三种块的选择和编码。
- 增量输入、受限输出、同步刷新、结束流，以及失败或取消后的资源清理。
- 公共 CRC32 组件，迁移脚本 CRC32 调用方并保留现有脚本结果。
- `GzipResponseWriter` 接入、CMake 依赖清理、测试参考实现隔离、参考 Nginx 构建脚本调整。
- 正确性、流式时延、内存和性能验收。

本次不提供生产解压 API、zlib 格式封装、Adler32 公共 API、预置字典、运行期调整参数、流复制、`gzFile`/文件 I/O、ZIP/minizip、其他压缩算法。raw DEFLATE 先作为私有实现，不额外承诺公共 API。HPACK/QPACK 的 Huffman 实现不在本次重构范围。

测试需要独立解压器：选择将 1.3.2 的参考编解码源文件保存在测试专用目录，直接编译为测试对象，不构建或链接上游 zlib 库。它不进入产品目标，也不作为生产解压能力。这样默认测试仍能验证任意动态生成的响应，无须依赖系统 zlib、Python 或外部 gzip 命令。

源码迁入意味着本项目承担所保留算法的维护责任；第三方来源和许可证仍然存在。

## 2. 当前代码事实

| 位置 | 当前行为 | 迁移影响 |
| --- | --- | --- |
| `cmake/Deps.cmake` | 固定下载 1.3.1；无条件准备源码；按需创建静态库目标 | 删除生产 FetchContent 声明及准备函数 |
| `CMakeLists.txt` | `GzipResponseWriter.cpp` 从 `fiber_lib` 中排除，由 `fiber_http_compression` 链接 zlib | 保留 HTTP 组件名称，替换其底层实现依赖 |
| `include/fiber/http/GzipResponseWriter.h` | 已通过私有 `CompressionState` 隐藏 `z_stream`；仍声明 workspace 回调 | 保持公开 writer 使用方式，删除分配回调 |
| `src/http/GzipResponseWriter.cpp` | 唯一直接调用 zlib 的生产源文件 | 替换初始化、压缩、flush、finish、析构 |
| `apps/lite_nginx/tests/LiteNginxRuntimeTest.cpp` | `gunzip_body()` 使用 `inflateInit2/inflate/inflateEnd` | 改用测试参考适配器 |
| `apps/lite_nginx/CMakeLists.txt` | 测试直接链接 `ZLIB::ZLIBSTATIC` | 改为测试专用对象依赖 |
| `scripts/build_nginx.sh` | 使用 `temp/_deps/zlib-src`，假设主项目已准备源码 | 工具自行准备固定版本的参考源码 |
| `src/http/Huffman.{h,cpp}` | HPACK/QPACK 固定码表和编译期解码表 | 保持原实现 |
| `src/script/std/Crc32.h` | 脚本私有、逐字节查表的标准 CRC-32 | 提升为 common 公共组件并迁移调用方 |

当前 writer 使用 16 KiB 输出缓冲，并在累计处理 256 KiB 输入后协作让出 EventLoop。zlib 分配已经接入 `exchange.pool()`，因此迁入源码不能被描述为首次消除逐次堆分配。

现有 workspace 公式为 `8192 + 16 + 2^(15+2) + 2^(8+9)`，即 270,352 字节；加输出缓冲约 280 KiB，尚未计入 writer 状态和池管理开销。当前按分配大小猜测 zlib 状态结构并预留 8 KiB，迁移后改为明确布局。

## 3. 上游基线与来源管理

### 3.1 固定输入

- 官方发布版：zlib 1.3.2，2026-02-17 发布。
- 官方归档：<https://zlib.net/zlib-1.3.2.tar.gz>。
- SHA-256：`bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`。
- 对照 tag：<https://github.com/madler/zlib/tree/v1.3.2>。

编写本文时已下载官方归档并验证上述哈希；仓库现有 1.3.1 缓存未修改。哈希只对应该官方 tar.gz，不能用于 GitHub 自动生成的 tag 归档。[官方发布信息](https://zlib.net/)

新增 `src/compression/UPSTREAM.md`，记录归档版本和哈希、每个派生文件的来源、所保留函数、删除的能力、语义变化、测试及后续上游修复合入记录。测试参考目录另有独立 manifest，记录文件哈希及构建配置。禁止用未固定版本的 `current` 下载地址。

### 3.2 1.3.2 差异处理

迁移时直接依据 1.3.2 的函数和状态定义，不能沿用旧版的结构尺寸估计。该版本含参数校验、状态初始化和复制相关修正，以及参考解压器固定表初始化方面的变化。`deflateCopy`、`inflateCopy`、bound/combine 等未暴露能力分别记录为未迁入或仅存在于隔离参考实现中，不能将整份发布说明当作当前生产路径的漏洞清单。[1.3.2 ChangeLog](https://github.com/madler/zlib/blob/v1.3.2/ChangeLog)

第一阶段保留 1.3.2 的匹配搜索参数、长度/距离映射、码长限制算法和 pending/symbol 缓冲重叠不变量。不能在类型转换或移除宏时顺便改写这些算法。

### 3.3 许可证

保留上游版权和完整许可证，在派生源文件明确标记“由 zlib 1.3.2 修改而来”，并将原来指向 `zlib.h` 的许可提示更新为有效的本地许可证路径。新增 `src/compression/LICENSE.zlib`；CRC32 派生实现和测试参考目录均能追溯到完整声明。不得将派生实现标为全部原创。[zlib 许可证](https://zlib.net/zlib_license.html)

## 4. 模块与构建边界

计划文件布局：

```text
include/fiber/compression/GzipEncoder.h
include/fiber/common/util/Crc32.h
src/compression/GzipEncoder.cpp
src/compression/DeflateEncoder.h       # 私有 raw DEFLATE 接口
src/compression/DeflateEncoder.cpp    # 滑动窗口、匹配、块驱动
src/compression/DeflateState.h        # 私有状态和 workspace 布局
src/compression/DeflateTrees.cpp      # 码长、规范码、块成本及位输出
src/compression/DeflateTables.h       # 固定表，私有
src/compression/UPSTREAM.md
src/compression/LICENSE.zlib
src/common/util/Crc32.cpp
src/common/util/detail/Crc32Tables.h  # 私有常量表
tests/Crc32Test.cpp
tests/GzipEncoderTest.cpp
tests/DeflateEncoderTest.cpp
tests/GzipResponseWriterTest.cpp
tests/support/ZlibReference.h
tests/support/ZlibReference.cpp
tests/support/third_party/zlib_1_3_2/  # 原样参考源文件、声明、manifest
cmake/FiberZlibReference.cmake
scripts/prepare_zlib_reference.sh
```

构建关系：

```text
fiber_lib
  ├─ common CRC32
  └─ GzipEncoder + 私有 DEFLATE 核心

fiber::http_compression
  └─ GzipResponseWriter → fiber_lib

fiber_tests / lite_nginx_tests
  └─ 测试参考对象 + ZlibReference 适配器
```

选择将无外部依赖的基础编解码组件编入现有 `fiber_lib`，让 `tests/*Test.cpp` 和普通库消费者可以直接使用；HTTP writer 仍留在独立组件中，保留 `FIBER_BUILD_HTTP_COMPRESSION` 和 `fiber::http_compression` 的现有用途。新增基础组件随 `src/*.cpp` 的递归收集进入 `fiber_lib`，只继续排除 writer。

公开头只依赖 `include/fiber/` 和标准库。私有 DEFLATE 头通过 `src` 私有 include root 使用；为 HTTP 压缩目标增加私有 include root 只能在确实必要时进行，优先只调用公共 `GzipEncoder`。测试参考源目录和宏均不得 PUBLIC 传播。

本次不增加永久双后端或运行期切换选项。性能基线使用独立构建的旧版程序或测试参考实现，不让迁移后的产品继续链接旧库。

## 5. 算法迁入清单

| 上游来源 | 生产保留内容 | 处理方式 |
| --- | --- | --- |
| `deflate.c`、`deflate.h` | 窗口、哈希链、fast/slow 匹配、lookahead、块驱动 | 转入私有 C++ 实现，固定当前 HTTP 参数 |
| `trees.c`、`trees.h` | 频率统计、受限码长、规范码、成本估计、三种块输出 | 保留算法和数组布局，收敛宏和可见性 |
| `crc32.c`、`crc32.h` | CRC32 更新和所需静态表 | 放入 common；保留可移植优化路径 |
| gzip header/trailer 分支 | 单 gzip member 封装 | 在 GzipEncoder 中实现显式分段输出 |
| `zutil.*`、`zconf.h`、`zlib.h` | 必需的类型和常量含义 | 转为固定宽度类型、私有常量和项目错误，不进入生产头依赖 |
| inflate、Adler32、其他公开 zlib API | 生产路径不用 | 仅按参考测试需要保存，不进 fiber_lib |

删除等级 0 的公开入口，不等于删除 stored block：不可压缩数据仍需要成本比较后的原样块。仅删除 RLE/Huffman-only 等策略的选择和专属路径，保留 default strategy 实际可达的全部功能。

不机械替换所有 `unsigned long`。窗口位置、距离、频率和位累加器分别选择有证明的宽度；跨平台 `size_t` 和 gzip 长度字段另行处理。所有移位、窄化、未对齐读写和数组边界都需要检查。输入仍须复制到历史窗口，不能声称压缩本身实现零拷贝。

## 6. 公共流式接口

以下为 `fiber::compression` 下的设计接口，具体声明需包含完整头文件和命名空间；公开类型不含私有状态定义。

```cpp
struct GzipEncoderOptions {
    int compression_level = 1;
};

enum class EncodeStatus : std::uint8_t {
    NeedInput,
    NeedOutput,
    Flushed,
    Finished,
};

struct EncodeStep {
    std::size_t consumed = 0;
    std::size_t written = 0;
    EncodeStatus status = EncodeStatus::NeedInput;
};

class GzipEncoder final : public common::NonCopyable,
                          public common::NonMovable {
public:
    static common::IoResult<GzipEncoder *> create(
        mem::BufPool &pool, GzipEncoderOptions options) noexcept;
    ~GzipEncoder() noexcept;

    common::IoResult<EncodeStep> write(
        std::span<const std::uint8_t> input,
        std::span<std::uint8_t> output) noexcept;
    common::IoResult<EncodeStep> flush(
        std::span<std::uint8_t> output) noexcept;
    common::IoResult<EncodeStep> finish(
        std::span<std::uint8_t> output) noexcept;
    void abort() noexcept;

private:
    struct State;
    explicit GzipEncoder(State &state) noexcept;
    State *state_;  // 成功构造后始终非空
};
```

选择分别提供 write/flush/finish，使“提交最后一段输入”和“开始结束操作”的边界明确，避免调用方在输出耗尽时重新提交最后一段输入。私有 raw 核心采用相同的进度模型，但不负责 CRC32 和 gzip 封装。

### 6.1 进度和缓冲契约

- `consumed <= input.size()`，`written <= output.size()`；flush/finish 的 consumed 恒为 0。
- 只消费返回 consumed 指定的输入。未消费部分由调用方重新提交，已消费部分允许调用方立即释放。
- 调用返回后不持有外部输入、输出指针；跨调用所需数据仅保存在工作区。输入和输出不得重叠，这是调用前置条件。
- `NeedInput` 只从 write 返回，表示本次输入已全部接收且当前普通写入可以等待新输入。允许核心持有 lookahead 或尚未形成块的 token，不能据此推断下游已看见全部明文。
- `NeedOutput` 表示需要继续提供输出空间，可能同时还有未消费输入。调用方先提交本次 written，再以剩余输入和新输出空间续调。
- `Flushed` 只从 flush 返回；`Finished` 只从 finish 返回。终态可以伴随最后一批 written，调用方必须先提交这些字节。
- 首轮支持输出容量 1 字节；空输出在确实需要输出时返回 `NeedOutput` 且不丢失数据。已完成或无数据的幂等操作允许直接返回对应终态。
- 每个成功调用必须消费输入、产生输出、进入新的内部阶段，或明确返回等待/完成状态。测试检查重复零进度，调用方不能无条件忙循环。
- 正常的输入不足、输出不足、无待刷新数据不映射为 `IoErr::Invalid`。

### 6.2 操作状态与幂等性

| 状态 | write | flush | finish | abort/析构 |
| --- | --- | --- | --- | --- |
| Active | 接收输入 | 开始同步刷新或直接 Flushed | 开始结束 | 安全终止 |
| Flushing | Busy | 续调至 Flushed，然后回 Active | Busy | 安全终止 |
| Finishing | Already | Already | 续调至 Finished | 安全终止 |
| Finished | Already | Already | Finished，零新增输出 | 安全、幂等 |
| Aborted | Canceled | Canceled | Canceled | 安全、幂等 |

以上 Busy/Already/Canceled 均通过 `IoResult` 表达。参数或顺序错误在修改状态前返回；内部不可恢复错误终止当前编码器，调用方不得重试流。仅将不能从操作阶段或 pending 游标推导的信息保存为成员，避免镜像多套布尔状态。

write 接收零长度输入不会隐式 finish；若尚未接收任何输入，也不因空 write 开始输出 header，直接 NeedInput。流已经启动时，空 write 可以继续排空已有 pending。首次 flush 前没有输入时直接 Flushed；flush 完成后没有新输入再次 flush 也直接 Flushed，不再添加标记。空流调用 finish 必须输出合法空 gzip member。

flush 进入后先完成当前块，再只生成一次同步标记，最后将 pending 输出排空才返回 Flushed。不能仅靠“输出空间还有剩余”判定完成。标记必须先写入内部 pending 区，再允许分 1 字节输出，避免重复标记和极小缓冲下无法完成的问题。flush 保留历史字典，不等同于 full flush。

finish 开始后不再接收新输入；依次结束最后 DEFLATE 块、补齐字节边界、输出 gzip trailer。即使最后一个字节刚好填满输出，也要根据阶段返回 Finished。析构和 abort 只清理状态，不隐式压缩、不写网络、不要求已经成功 finish。

### 6.3 gzip 封装

每个响应只生成一个 member。固定 magic、CM=8、FLG=0、MTIME=0，无 filename/comment/extra/header CRC；XFL 按等级 1、9、其他分别为 4、2、0。OS 字段保持 1.3.2 在项目支持平台上的默认映射：Linux/Unix 为 3，Apple 为 19，通过私有编译期常量表达，不依赖 zlib 宏。跨平台比较时单独识别该字段，不承诺跨平台压缩结果逐字节相同。

header 和 trailer 使用固定数组及游标支持任意输出分片。CRC32 与 ISIZE 只根据本次确实 consumed 的明文字节更新。ISIZE 使用 `uint32_t` 按模 2^32 累计；诊断统计另用宽计数，不混用饱和计数。trailer 的 CRC32 和 ISIZE 均按 little-endian 输出。[RFC 1952](https://www.rfc-editor.org/rfc/rfc1952.html)

## 7. 内存、生命周期与调度

`create()` 先校验等级，再计算对齐后的总尺寸，一次向 BufPool 申请编码器、状态和固定 workspace；失败不发布半初始化指针。成功路径建立非空指针不变量，内部调用不反复检查。

固定参数下的主要 workspace 为：

| 区域 | 目标大小 |
| --- | --- |
| 双窗口 | 64 KiB |
| prev 哈希链 | 64 KiB |
| head 哈希表 | 64 KiB |
| pending/token 重叠区域 | 64 KiB |
| 状态、树、gzip 小缓冲和对齐 | 按真实 sizeof/alignof 计算 |

保留默认非 `LIT_MEM` 的 pending/token 重叠策略，不将重叠区域误拆成重复分配的两块。保留窗口已初始化范围相关约束；不能以“热路径优化”为由删除初始化保护。

BufPool 负责回收内存，但不会自动调用对象析构。writer 持有成功创建的编码器指针，在池销毁前用 `std::destroy_at()` 结束对象生命周期；不使用 delete。正常 finish、失败、abort、从未激活压缩四条路径分别覆盖。第一次实现不新增跨请求缓存或按 worker 常驻池，避免改变驻留内存策略。

核心完全同步，不访问 EventLoop，不执行 I/O，不产生协程。writer 保持 256 KiB 输入预算和既有让出行为；按实际 consumed 累计，不按提交长度累计。等级 9 的搜索可能使相同字节预算耗时更长，验收同时测 EventLoop 延迟；若需要缩小预算，应作为有数据依据的独立调整。计时使用 `EventLoop::current().now()`。

## 8. 基础组件复用

### 8.1 CRC32

新增 `fiber::util::Crc32`，沿用现有 common/util 组件的命名空间，提供默认构造、`reset()`、`update(span)`、`value()` 和一次性计算入口。公共语义统一为标准 CRC-32：空输入结果 0，`123456789` 结果 `0xCBF43926`；内部实现独立处理初始值及最终异或，不向脚本调用方暴露半完成状态。

生产 CRC32 后端以 1.3.2 可移植优化实现为基础，支持 32/64 位和字节序，保留逐字节尾部处理；不启用依赖外部贡献目录的特定平台向量实现。使用静态常量表，避免首次请求动态建表。表生成若迁为 constexpr，需检查编译耗时，不能盲目复制 HPACK 的巨大编译期表生成方式。

`HashFuncs.cpp` 和 `RandFuncs.cpp` 迁移到公共组件，删除旧 `src/script/std/Crc32.h`。校验文本强制转换、空值、分段拼接和 canary 取模结果全部保持。CRC32 与 CRC32C 不可互换；后续硬件加速必须明确多项式。

独立比较短 key 和大 body 的性能。如果迁入优化路径显著增加小字符串开销，应在公共实现内部使用短输入路径，不能保留两份对外语义不同的 CRC。

### 8.2 Huffman 和堆

HPACK 使用固定码表；DEFLATE 使用字面量/长度树、距离树和码长树，还需要频率建树、码长限制及动态树描述。保留 DEFLATE 专有建树及位输出代码，不让基础压缩组件依赖 `src/http/Huffman.h`。[RFC 7541](https://www.rfc-editor.org/rfc/rfc7541.html#appendix-B)、[RFC 1951](https://www.rfc-editor.org/rfc/rfc1951.html)

本轮只复用明确通用且收益可证明的设施，不新增同时服务两种协议的大型 Huffman 抽象。zlib 建树使用紧凑数组和索引堆；现有 intrusive BinaryHeap 带多个指针，不直接替换，以保持缓存局部性。后续抽取通用工具必须独立证明 HPACK/QPACK 无性能回退。

## 9. GzipResponseWriter 迁移

保留 eligibility、Accept-Encoding、status/content-type 策略、header 过滤、stats、HTTP/1/2/3 完成语义和已有选项。`request_accepts_gzip`、`all_body_statuses` 等可复用组件选项不删减。

具体修改：

1. 删除 `<zlib.h>`、`z_stream`、`workspace_alloc/free`、workspace 尺寸猜测和 initialized 镜像状态。
2. 延续现有按需激活压缩的时机，分配输出 IoBuf 后创建 GzipEncoder；逐项检查失败清理。
3. `compress_input()` 按输入预算切片，循环 write，按 consumed 前进、按 written commit；NeedOutput 时 drain，输入尚未消费完不得返回。
4. `sync_flush()` 循环 flush 至 Flushed，然后排空输出并调用下游 flush。业务层“没有新写入”的快捷判断可以保留，但编码器本身也必须正确处理重复 flush。
5. `finish()` 先保留现有 expected input length 校验，再循环至 Finished，最后使用现有 `end_stream` 参数通知下游。流结束标志只发送一次。
6. 下游短写、错误和取消仍由 writer 处理；编码器 abort 后不再尝试补尾，也不能改为明文响应。已发出 gzip headers 的响应发生失败时按既有协议错误路径结束。
7. 明文输入统计按实际接收量计算；输出统计沿用下游成功写出量，不把编码器生成但尚未发送的数据当作已发送。

保留链式输入逐段消费，不将 IoBufChain flatten 为连续临时字符串。输出继续使用 16 KiB 可复用 IoBuf，背压等待期间编码器和借用的数据所有者必须仍然存活。

公共 writer 的私有布局可能变化，要求静态库消费者重新构建；保持 C++ 源码调用方式和 CMake target 名称，不承诺旧目标文件的 ABI。

## 10. 测试参考实现与依赖清理

### 10.1 默认测试参考实现

`tests/support/third_party/zlib_1_3_2/` 保存来自同一官方归档的必要文件：

- C 源：`deflate.c`、`trees.c`、`inflate.c`、`inftrees.c`、`inffast.c`、`crc32.c`、`adler32.c`、`zutil.c`。
- 配套头：`zlib.h`、`zconf.h`、`zutil.h`、`deflate.h`、`trees.h`、`inflate.h`、`inftrees.h`、`inffast.h`、`inffixed.h`、`crc32.h`、`gzguts.h`。编写本文时已用 `cc -MM` 核验这些 C 源在当前 Linux 默认配置下的头依赖；实施时再验证其他支持平台和最终宏配置。
- LICENSE、文件哈希 manifest 和本地集成说明。

保留参考算法原样，使用测试专用 CMake OBJECT target 编译，编译特性和宏仅对该目标生效。通过强制包含的测试前缀映射头，将全部外部可见 zlib 符号改为 `fiber_test_zlib_*`；使用 nm 核验，不能只给 inflate/deflate 两个函数加前缀。无需修改原始文件来添加 include。

不使用上游 CMake，不生成 `ZLIB::ZLIBSTATIC`，不安装参考头，不将源目录加入生产目标。关闭动态生成的 CRC/fixed 表路径，使用发布包静态表，避免测试引入不必要的初始化差异。生产核心与参考实现不共用 CRC32、建树或状态代码，降低相同修改导致测试互相掩盖的风险。

`ZlibReference` 适配器提供完整解压、增量解压和固定参数压缩。测试结果明确返回成功/失败、consumed、是否 stream end；禁止使用空字符串同时表示“合法空响应”和“解压失败”。完整验证必须检查 trailer 和无剩余输入；同步刷新验证允许尚未达到 stream end，但必须已经读到全部预期明文。

测试参考实现中的压缩入口用于差分和基准；它不意味着产品保留旧后端。可额外用系统 gzip 或其他独立解压器做互操作验证，但不以它们存在与否决定默认核心测试是否执行。

### 10.2 主构建清理

- 删除 `FIBER_ZLIB_VERSION/URL/SHA256/SOURCE_DIR` 的生产依赖配置、zlib FetchContent 和 `fiber_prepare_zlib_target()`。
- 删除产品及测试对 `ZLIB::ZLIBSTATIC` 的引用；保留 protobuf 已有的 `protobuf_WITH_ZLIB=OFF`。
- 保留现有其他依赖的缓存逻辑，不能因删除 zlib 而改动 BoringSSL/protobuf 的来源。
- 新测试放在 `tests/*Test.cpp`，按当前 glob 纳入 fiber_tests；writer 测试源在 `FIBER_BUILD_HTTP_COMPRESSION=OFF` 时从目标移除，开启时给 fiber_tests 增加 writer 目标依赖。
- 只有构建测试时才创建参考对象目标；lite_nginx_tests 和 fiber_tests 分别私有链接该对象及适配器。
- 更新 `README.md` 的外部依赖表及 `FIBER_FETCH_DEPS=OFF` 说明，移除主配置仍准备 zlib 源码的旧描述。
- 更新仍指导用户设置 `FETCHCONTENT_SOURCE_DIR_ZLIB` 的构建文档。历史性能报告保留历史命令，可注明已不适用于新构建，不伪造历史基线。

### 10.3 Nginx 参考构建

新增 `prepare_zlib_reference.sh`，使用第 3 节的 URL 和哈希，将完整参考源码准备到 `temp/zlib-reference/1.3.2/`；使用临时文件下载、验证后落盘，并验证缓存版本。`build_nginx.sh` 按需调用该脚本，更新 `--with-zlib` 路径及 usage 注释，不再要求主 CMake 预先下载 zlib。

完整参考源码只服务开发工具，不进入产品配置路径。参考 Nginx 编译会修改其 zlib 源目录，因此与默认测试的受版本控制只读源文件分开保存。保留 BoringSSL 的既有准备前提，不宣称本次把整个参考构建改成完全独立。

## 11. 测试和验收矩阵

### 11.1 算法与接口

| 维度 | 必测内容 |
| --- | --- |
| 输入 | 空、单字节、所有字节值、重复串、JSON/HTML、UTF-8、确定种子随机数据、已压缩数据 |
| 等级 | 1～9 全覆盖；非法 0、负数、10 初始化失败 |
| 分片 | 输入 1/2/3 字节及随机分片；输出 0/1/2/5/6/7 字节、16 KiB 和随机容量 |
| 窗口 | 32 KiB/64 KiB 附近、重复跨窗口、最短/最长匹配、远距离匹配 |
| 块 | stored/fixed/dynamic 均有可验证触发样本；动态码长限制和稀疏频率 |
| flush | 无输入、连续重复、每个输入片段后、标记被逐字节拆开、精确填满输出 |
| finish | 空流、最后输入先完全消费、多次续调、重复完成、完成后写入 |
| 校验 | CRC 已知值及随机分片；独立参考验证 CRC/ISIZE；ISIZE 跨 2^32 回绕 |
| 错误 | 非法调用顺序、初始化分配失败、abort 后调用、未 finish 析构 |
| 内存 | steady-state 无分配；无外部 span 悬挂；workspace 边界和未对齐访问 |

默认快速测试以私有计数边界测试验证 ISIZE 回绕；另设扩展测试，用固定小缓冲循环输入超过 4 GiB 并流式解压/校验，避免分配巨型字符串。扩展测试必须在迁移验收运行一次。

差分以“参考解压得到原文、协议合法、无尾部丢失”为硬标准。对相同平台、相同参数、相同操作序列做压缩结果比较，任意字节差异需要解释和记录。小输出缓冲下有意消除重复 flush marker 可能改变字节序列，不因此判定格式不兼容。跨平台 OS 字段差异按第 6.3 节处理；压缩率回退另按性能门槛判断。

### 11.2 HTTP 集成

保留并迁移现有回归，包括：

- `AppliesInheritedGzipToScriptResponse`。
- `GzipWriterUsesNativeHttp3StreamCompletion`。
- `HttpProxyPassFlushControlsGzipSyncFlush`。
- `ProxyBufferingOffSyncFlushesChunkedUpstreamGzipResponse`。

补充 HTTP/1、HTTP/2、HTTP/3 的空响应和结束标记，HEAD/204/304 等绕过行为，已编码响应，已知/未知长度，长度不足/超出，脚本响应和代理响应，以及下游失败或取消。

流式测试通过可控上游屏障证明：在上游尚未发送下一段或 EOF 时，下游已解压出当前段。不能只在 EOF 后对完整 body 做 gunzip 来声称同步刷新通过。失败和取消路径必须证明不追加第二份 trailer 或再次发送 end。

### 11.3 构建与公共边界

- 默认 build + CTest 通过；所有新增测试实际被发现和执行。
- `FIBER_BUILD_TESTS=OFF` 的生产构建不含测试参考对象。
- `FIBER_BUILD_APPS=OFF`、HTTP 压缩组件 ON/OFF 分别构建，证明核心库和 writer 开关关系正确。
- 其他依赖已缓存时，使用全新构建目录、无旧 zlib 缓存和 `FIBER_FETCH_DEPS=OFF` 完成配置/构建；不能把其他依赖的下载失败归因于 zlib。
- 独立 consumer 只通过公开 include 和 `fiber_lib` 使用 GzipEncoder；writer consumer 通过 `fiber::http_compression` 构建。
- 检查实际链接命令、归档对象和最终程序符号，不只检查 `ldd`；确认产品不存在 zlib C API 依赖、测试前缀或 ZLIB target。
- 更新后的参考 Nginx 准备/构建流程单独验证，不能以产品无 zlib 下载为由漏掉工具回归。

### 11.4 Sanitizer 与性能

对核心及随机分片运行 ASan/UBSan，关键窗口/重叠访问尽可能再做 MSan。使用隔离构建目录；不要复用会混入不同工具链和 sanitizer 选项的依赖构建产物。

基线同时保留当前 1.3.1 writer 和未修改的 1.3.2 参考压缩器，以区分上游升级和本地迁移的影响。测等级 1/6/9、短响应/长响应、可压缩/不可压缩数据、普通输出/频繁 flush；记录吞吐、CPU 时间、输出大小、分配次数、并发 RSS、首块与 EventLoop p95/p99 延迟。

建议作为本轮实现验收门槛：

- 相同环境重复至少 5 轮，关键吞吐中位数下降不超过 5%。
- 相同语料的输出大小原则上保持；聚合回退超过 1% 必须定位原因，小输入同时报告绝对字节差异。
- 单编码器主要 workspace 不超过当前约 264 KiB 基线，16 KiB 输出缓冲另计；记录真实分配含管理开销的结果。
- 单次初始化后 write/flush/finish 不新增动态分配。
- 流式可见性测试必须通过；时延有稳定回退时继续定位，不能用吞吐达标替代。

这些是计划门槛，不是已经测得的收益。若硬件噪声高，应报告离散程度并复测，不能事后放宽门槛掩盖退化。

## 12. 分阶段实施与完成条件

| 阶段 | 主要修改 | 阶段完成条件 |
| --- | --- | --- |
| A：基线和来源 | 固定 1.3.2、许可证、来源映射、测试参考目标、迁移 gunzip helper | 旧生产 writer 与新测试参考同时通过现有 gzip 回归，保存性能基线 |
| B：CRC32 | 公共接口、1.3.2 可移植后端、脚本调用方迁移 | CRC 差分、脚本 CRC/canary 回归和短/长输入基准通过 |
| C：私有 DEFLATE | 工作区、匹配、树、块、流式进度 | 三种块、窗口边界、等级 1～9、随机分片和 sanitizer 通过 |
| D：GzipEncoder | header/trailer、CRC/ISIZE、操作状态机 | 空流、重复 flush、1 字节输出、finish/abort 和独立解压通过 |
| E：HTTP 接入 | 替换 writer 的所有 zlib 调用和生命周期代码 | writer 及 HTTP/1/2/3 集成、流式可见性、取消回归通过 |
| F：依赖与工具 | 删除生产 zlib 配置、调整参考脚本和文档 | 新构建目录无 zlib 外部依赖；默认测试完整；参考工具可运行 |
| G：最终验收 | 全 CTest、扩展测试、性能对照、consumer 构建 | 第 11 节证据完整，UPSTREAM 映射与交付实现一致 |

阶段 A～D 可以保留旧 writer 作为开发中的对照，但它不是最终交付状态；E～G 未完成不能称为迁移完成。每阶段保持独立、可审查的改动，避免将上游算法搬移与性能重写混为一个 diff。

完成后运行 `./format_code.sh`；参考第三方目录保持原始格式。运行 `git diff --check`，再执行有针对性的构建和测试。计划命令如下，实施时记录实际构建目录、配置和输出，不把本节命令当作已执行证据：

```bash
cmake -S . -B build
./format_code.sh
git diff --check
cmake --build build --target fiber_tests lite_nginx_tests
ctest --test-dir build --output-on-failure -R 'Crc32|DeflateEncoder|GzipEncoder|GzipResponseWriter|HashFuncs|RandFuncs|Gzip'
cmake --build build
ctest --test-dir build --output-on-failure
```

若某项测试因环境缺失不能执行，必须列明未验证项，不用其他 focused tests 代替。未来正式验收还需按实际新增测试名称补齐过滤器及扩展测试命令。

## 13. 风险与后续维护

| 风险 | 控制措施 |
| --- | --- |
| 搬移时破坏 pending/token 重叠或位操作 | 保留上游不变量，分步差分，ASan/UBSan 和极小输出测试 |
| flush 只有 EOF 后才能解压 | 明确 Flushing 阶段和单次标记生成，使用非 EOF 屏障测试 |
| CRC 统一后拖慢请求路径 | 同测短 key、大 body，保留公共接口下的短输入路径 |
| 取消时仍要求 deflateEnd 成功 | 析构不要求 Finished，abort 不输出，并单独验证池生命周期 |
| 测试参考实现进入产品 | 私有对象目标、独立 include、全部符号前缀、最终链接审计 |
| 与上游分叉后修复难以合入 | 逐文件来源映射，算法与适配分开提交，逐条分析上游发布差异 |
| 下游依赖旧 CMake 或目标文件 | 保留公开目标名，更新构建说明，要求完整重编译 |

后续 zlib 发布时，对保留的压缩、CRC 和测试参考代码分别评估修复适用性；记录“已合入 / 不适用及原因 / 待处理”。性能优化、生产解压器、其他封装和通用 Huffman 抽取另立方案，不随本次迁移隐式扩展。
