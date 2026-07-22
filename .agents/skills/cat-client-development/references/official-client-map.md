# Official CAT 3.0 Client and Server Map

The compatibility baseline is official Dianping CAT `v3.0.0`, commit `f875ff10b1a3f2922fef1bfca7ba34c54805b021`. The preparation script checks out only `lib/c` and `lib/cpp` from `https://github.com/dianping/cat.git` into `temp/cat`. Cone-mode sparse checkout also retains repository-root metadata such as the license.

Run the preparation script without `--ref` before using this map. It enforces the pinned baseline. The CAT 3.0 Java client and server sources remain available as Git objects even though they are outside the sparse working tree; inspect them with `git -C temp/cat show HEAD:<path>`.

## Start Here

| Question | Official source |
| --- | --- |
| Public C API and configuration | `lib/c/include/client.h`, `lib/c/src/ccat/client.c`, `client_config.c` |
| Public C++ facade and RAII behavior | `lib/cpp/include/client.hpp`, `lib/cpp/src/cppcat/*.cpp` |
| Current transaction/message-tree context | `lib/c/src/ccat/context.c`, `message_manager.c`, `message_tree.c`, `transaction.c` |
| Message, root, and parent IDs | `lib/c/src/ccat/message_id.c`, `message_helper.c`, `message_tree.h` |
| Encoder selection and framing | `lib/c/src/ccat/encoder.c`, `encoder_binary.c`, `encoder_text.c` |
| Router HTTP request and collector selection | `lib/c/src/ccat/server_connection_manager.c`, `router_json_parser.c` |
| Sender queue, reconnect, and socket writes | `lib/c/src/ccat/message_sender.c`, `server_connection_manager.c` |
| Sampling and aggregation | `lib/c/src/ccat/aggregator.c`, `aggregator_event.c`, `aggregator_metric.c`, `aggregator_transaction.c` |
| Client heartbeat and self-monitoring | `lib/c/src/ccat/monitor.c`, `monitor_collector.c` |
| Low-level queues, networking, buffers, maps | `lib/c/src/lib/` |
| Upstream tests and examples | `lib/c/tests/`, `lib/cpp/tests/`, `lib/cpp/scripts/` |
| Java message-ID generator and parser | `cat-client/src/main/java/com/dianping/cat/message/internal/MessageIdFactory.java`, `MessageId.java` |
| Java NT1/PT1 receiver codecs | `cat-client/src/main/java/com/dianping/cat/message/spi/codec/NativeMessageCodec.java`, `PlainTextMessageCodec.java` |
| Server collector decode entry | `cat-core/src/main/java/com/dianping/cat/message/CodecHandler.java` |
| Log View request and storage lookup | `cat-home/src/main/java/com/dianping/cat/report/page/logview/Handler.java`, `service/LocalMessageService.java` |

The C++ tree links `lib/cpp/src/ccat` and `lib/cpp/src/lib` back to the C implementation. Always prepare both directories so those links resolve.

## Compatibility Anchors

- `server_connection_manager.c` constructs the router request under `/cat/s/router` and parses collector endpoints, block state, and related configuration.
- `encoder_binary.c` writes the `NT1` format; `encoder_text.c` writes `PT1`. Verify field order, null handling, integer encoding, timestamps, message nesting, and escaping from code rather than from format names alone.
- `message_manager.c` and `message_helper.c` decide when a tree receives message IDs and how child propagation rewrites message, root, and parent IDs.
- CAT 3.0 message IDs have the visible shape `<domain>-<ipHex[.pid]>-<hour>-<index>`. The Java parser scans from the right, parses both `hour` and `index` with `Integer.parseInt`, and rejects negative values. Keep generated values in the non-negative Java `int` range even if fiber2 stores them in wider types.
- The official CAT 3.0 C client uses an `int` message-ID index. Its optional multiprocessing form adds the PID to the IP component instead of widening the index.
- CAT 3.0 Log View parses the message ID before locating the storage bucket. An out-of-range numeric field can therefore leave URL aggregation visible while `/cat/r/m/<message-id>` fails with HTTP 500.
- `context.c` stores the active tree in `CATTHREADLOCAL` state. This is an upstream implementation detail, not a safe fiber2 design: coroutines sharing an event-loop thread can interleave across suspension points.
- `message_sender.c`, `aggregator.c`, and `monitor.c` create dedicated pthreads. Preserve their externally visible behavior, but replace their thread/lock/event-loop structure with fiber2 lifecycle and ownership primitives.
- The official low-level library uses SDS strings, malloc-oriented containers, locks, and its own socket event loop. Consult it for behavior and edge cases, not as the preferred memory or I/O layer.

## fiber2 Counterparts

Search these areas before introducing new infrastructure:

- Event-loop ownership and time: `src/event/EventLoop.*`
- Coroutine tasks, coordination, and timers: `src/async/`
- Raw collector transport: `src/net/TcpStream.*`, `src/net/detail/ConnectFd.*`
- Router HTTP transport: `src/http/Http1ClientConnection.*`, `src/http/ClientHttp1Exchange.*`
- Address resolution: `src/dns/`
- Router JSON decoding: `src/common/json/`
- Reusable output buffers: `src/common/mem/IoBuf.*`, `IoBufChain.*`, `BufPool.*`
- Metric storage and benchmark patterns: `apps/prometheus/`

## Focused Search Commands

Run searches from `temp/cat` and narrow before opening broad files:

```bash
rg -n 'CATTHREADLOCAL|g_cat_context|newTransaction|endTransaction' lib/c lib/cpp
rg -n 'NT1|PT1|encode|messageId|rootMessageId|parentMessageId' lib/c/src/ccat
rg -n '/cat/s/router|sample|block|2280|reconnect|send' lib/c/src/ccat
rg -n 'pthread_create|queue|drop|monitor|heartbeat' lib/c/src/ccat
git show HEAD:cat-client/src/main/java/com/dianping/cat/message/internal/MessageId.java
git show HEAD:cat-client/src/main/java/com/dianping/cat/message/spi/codec/NativeMessageCodec.java
git show HEAD:cat-home/src/main/java/com/dianping/cat/report/page/logview/Handler.java
```

Treat source comments and tests as supporting evidence. When they conflict with executed code, follow the CAT 3.0 code path and document the discrepancy. For data emitted by fiber2 and consumed by CAT, the CAT 3.0 receiver's accepted ranges and lookup semantics are part of the compatibility contract.
