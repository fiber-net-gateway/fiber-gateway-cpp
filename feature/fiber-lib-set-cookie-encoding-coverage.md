# 上游测试需求:HTTP 响应头 set-cookie(重复字段名)编解码覆盖

> 目标仓库:`fiber-gateway-cpp`(当前 pin 47d84785)
> 性质:**纯测试新增,不改产品代码**
> 建议提交:`test(http): cover set-cookie and duplicate field lines in h2/h3 response encoding`

## 1. 背景与动机

access-gateway 侧(2026-11-19)审计结论:响应端 set-cookie 链路(上游解析 → 代理头拷贝 → gzip 过滤 → h1/h2/h3 出口序列化 → QPACK/HPACK 编码)**功能上无缺陷**,多条 Set-Cookie 以独立字段行保留、不合并。但整个测试套件对此**零覆盖**:

- `tests/HttpHeadersTest.cpp:204` 只覆盖容器层(`add` 两条 `set-cookie`,`get` 返回第一条);
- `tests/CookieCodecTest.cpp` 只覆盖脚本函数 `set_cookie()` 的 cookie 序列化;
- `tests/Http3QpackEncoderTest.cpp`、`tests/Http3QpackDecoderTest.cpp`、`tests/Http2HpackEncoderTest.cpp` 无任何 set-cookie 用例;
- `tests/Http3ServerConnectionTest.cpp` / `tests/Http2ServerConnectionTest.cpp` / `tests/Http1ServerTest.cpp` 无任何重复字段名的响应往返用例。

Set-Cookie 是唯一**不允许逗号合并**的常见响应头(RFC 9110 §5.6)。一旦 QPACK 静态表 `find()` 桶索引重构、`ServerHttp3Request::send_header` 的字段遍历、或 `GzipResponseWriter::prepare_filtered_headers` 出现按名去重/覆盖回归,重复 Set-Cookie 会被**静默**合并或丢弃,现有测试全部拦不住。

## 2. 需求(按优先级)

### P1 — QPACK 编解码层(`tests/Http3QpackEncoderTest.cpp`)

沿用现有模式:`Http3QpackEncoderIoBufWriter` + `chain_to_bytes()` 精确字节断言 + `DecodeRecorder` 回读。name_hash 用 `http_header_name_hash("set-cookie")`。

**P1-1 `EncodesSetCookieUsingStaticNameReference`**
```cpp
writer.encode_field("set-cookie", http_header_name_hash("set-cookie"), "a=b");
```
- 精确字节:`{0x00, 0x00, 0x5E, 0x03, 'a', '=', 'b'}`
- `0x5E` = `0101 1110`:Literal Field Line with Static Name Reference(N=0,T=1,索引 14);`0x03` 为 raw 值长度(H=0,7-bit)
- `DecodeRecorder` 回读恰一条 `"set-cookie=a=b"`

**P1-2 `EncodesEmptySetCookieAsIndexedStaticField`**
```cpp
writer.encode_field("set-cookie", hash, "");
```
- 精确字节:`{0x00, 0x00, 0xCE}`(`0xC0|14`,indexed,值恰等于静态表条目的空值)

**P1-3 `EncodesRepeatedSetCookieAsSeparateFieldLines`(核心用例)**
```cpp
writer.encode_field("set-cookie", hash, "a=1; Path=/; HttpOnly");
writer.encode_field("set-cookie", hash, "b=2; Path=/; Secure");
```
- 断言输出包含**两个** `0x5E` 前缀字段行(或按 decode 断言两条独立字段)
- `DecodeRecorder` 回读恰两条,顺序 `"a=1; ..."`、`"b=2; ..."`,值内无追加逗号

**P1-4 `RoundTripsHuffmanEncodedSetCookieValue`**
- 长 cookie 值(典型字符,如 60+ 字节 `sid=` + 十六进制串)编码后 decode 还原相等
- 可选:断言该值走了 Huffman(H 位=1)且编码长度短于 raw;不强断言精确字节

### P2 — h3 服务端往返(`tests/Http3ServerConnectionTest.cpp`)

仿 `ServerCanSendFinalResponseHeader`(`tests/Http3ServerConnectionTest.cpp:233`)的 `ServerFixture` 模式,新增 `ServerPreservesRepeatedSetCookieInResponseHeader`:

1. handler 内:
   ```cpp
   fiber::http::HttpHeaders headers(exchange.pool());
   headers.add("set-cookie", "a=1; Path=/; HttpOnly");
   headers.add("set-cookie", "b=2; Path=/; Secure");
   headers.set("content-type", "text/plain");
   co_await exchange.send_header({.kind = Final, .status_code = 200, .headers = &headers, ...});
   ```
2. 客户端侧用 `ClientFixture`(`tests/Http3ConnectionTestSupport.h:118`)+ `ClientHttp3Request` 读响应头;若 harness 不便,退而取 server 写出的 HEADERS frame payload 用 `Http3QpackDecoder` 解码
3. 断言:
   - `get_all("set-cookie", hash)` 恰两条,顺序 `a=1; ...`、`b=2; ...`
   - status 200、content-type 正常(排除编码整体失败假阳/假阴)

### P3 — h2 / h1 / gzip 横向一致性

**P3-1 HPACK(`tests/Http2HpackEncoderTest.cpp`)**
- `set-cookie` 非空值走静态名引用(HPACK 静态表索引 55):首字节 `0x0F 0x28`(4-bit 前缀续位,55=15+40),后接 raw 值 `0x03 'a' '=' 'b'`(当前实现为 without-indexing,无动态表插入,字节稳定)
- 空 set-cookie:不在 HPACK 静态表 exact 命中范围(静态值为 `""`,空值**是否** exact-indexed 以 `Http2HpackStaticTable::find` 实现为准;用例按实际行为断言并在注释说明 RFC 7541 语义)
- 两条 set-cookie 独立字段行,decode 回读两条、顺序保留

**P3-2 h1 服务端序列化(`tests/Http1ServerTest.cpp`)**
- 响应含两条 Set-Cookie 时,出口原始字节流出现两行独立 `Set-Cookie: ...` CRLF,**无逗号合并、无首条丢弃**(序列化循环在 `src/http/Http1ExchangeIo.cpp:154`,逐字段输出)

**P3-3 gzip 过滤头(可选,现有 gzip writer 测试文件)**
- `GzipResponseWriter` active 路径下,两条 set-cookie 在 `prepare_filtered_headers` 后仍为两条(`src/http/GzipResponseWriter.cpp:350` 逐字段 `add_view`,set-cookie 不在过滤名单)

## 3. 验收标准

1. 上述用例全绿(纯新增,不动产品代码)。
2. **变异自检**:临时把 `HttpHeaders::add` 改为 set 语义、或把 `send_header` 的字段遍历改为按名去重,P1-3 与 P2 用例必须变红——否则覆盖无效。
3. 遵守上游规范:`./format_code.sh`、`./run_tidy.sh` 零告警;测试文件 `tests/*Test.cpp` 自动被 CMake 收进 `fiber_tests`,无需改构建。
4. 如后续本仓库 bump pin,access-server 侧无需适配(本需求不触碰产品行为)。

## 4. 参考事实(已对当前实现核对)

| 事实 | 出处 |
| --- | --- |
| QPACK 静态表 `set-cookie` = 索引 14(0-based) | `src/http/Http3QpackStaticTable.cpp:21`,RFC 9204 Appendix A |
| HPACK 静态表 `set-cookie` = 索引 55 | `src/http/Http2HpackStaticTable.cpp:88`,RFC 7541 Appendix A |
| QPACK 编码形式:静态名引用 `0x50`/4-bit,indexed `0xC0`/6-bit,字面名 `0x20`/3-bit,Huffman 位 `0x80` | `src/http/Http3QpackEncoder.cpp:275-296` |
| HPACK 编码形式:without-indexing `0x00`/4-bit(无动态表插入) | `src/http/Http2HpackEncoder.cpp:259-272` |
| h3 出口逐字段遍历 | `src/http/ServerHttp3Request.cpp:1331-1345` |
| 容器重复名保留(`next_all` 链) | `include/fiber/http/HttpHeaders.h:42-53` |
