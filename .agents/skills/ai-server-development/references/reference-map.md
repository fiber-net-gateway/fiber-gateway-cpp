# ploto-llm Reference Map

Use this map to select evidence for the current task. Resolve all paths relative to `/home/dear/CLionProjects/ploto-gateway` unless marked as C++ paths. Search with `rg` before opening broad directories, and trace call sites plus focused tests rather than inferring behavior from names.

## Assembly, Dynamic Configuration, and Lifecycle

- Documents: `documents/ai-injector-lifecycle.md`, `documents/fiber-net-gateway-index.md`, and `documents/ai-llm-nacos-debug-config.md`.
- Java entry points: `ploto-llm/src/main/java/com/haiercash/ploto/ai/llm/LlmModule.java` and the `server/LlmProjectHandle.java`, `server/LlmProjectRuntime.java`, and `server/LlmRouteConfigWatcher.java` paths.
- Follow config reference pools and compiled snapshots under `auth/group`, `model`, and `provider`.
- In C++, inspect `apps/nacos` and the owner-loop or watcher abstractions already present before choosing config publication and shutdown semantics.

## BT1 Authentication and Model Authorization

- Contract: `documents/ai-llm-auth-bt1.md`.
- Java implementation: `ploto-llm/src/main/java/com/haiercash/ploto/ai/llm/auth`, `auth/group`, and the authorization classes under `model`.
- Tests: matching paths under `ploto-llm/src/test/java/com/haiercash/ploto/ai/llm/auth` plus model and route tests that exercise group filtering.
- Preserve token parsing, signature input, key rotation, clock and expiry boundaries, log redaction, principal propagation, group matching, and error payload behavior.

## Model Compilation, Provider Selection, and Failover

- Contract: `documents/ai-llm-provider-selection.md`.
- Java implementation: `model/CompiledModelRegistry.java`, `model/CompiledModelRoute.java`, `model/CompiledLoadBalanceConfig.java`, and the complete `provider` package.
- Execution boundary: `server/OpenAiChatCompletionsExecutor.java` and `server/AnthropicMessagesExecutor.java`.
- Tests: `provider/ExecutionPlanResolverTest.java`, `provider/DefaultProviderErrorMarkerTest.java`, `provider/ProviderFailureClassifierTest.java`, `provider/ProviderRouteKeyExtractorTest.java`, and `server/ProtocolExecutorsTest.java`.
- Check route-key extraction, provider and token ordering, runtime-unavailable scope and TTL, `Retry-After`, retryable statuses, fallback, client-close gates, response-start gates, and exact attempt bounds together.

## OpenAI and Anthropic Wire Protocols

- OpenAI fields: `documents/openai-chat-completions-api.md`.
- Anthropic fields: `documents/anthropic-messages-api.md`.
- Java implementation: the `openai/chat` and `anthropic/messages` packages plus their protocol adaptors and executors under `server`.
- Preserve required fields, defaults, pass-through or unknown-field behavior, error schemas, usage extraction, header mapping, and streaming event order.
- In C++, inspect the HTTP exchange and client APIs under `src/http` and the JSON parser/encoder under `src/common/json`; avoid assuming a Java DTO should become an owning C++ object graph.

## Cross-Protocol Bridging

- Contract and staged scope: `documents/ai-llm-protocol-bridge-design.md`.
- Java implementation: the complete `bridge` package, `provider/ProtocolBridgeMode.java`, both server protocol adaptors, and both executors.
- Tests: bridge-focused tests and `provider/ProtocolProviderClientTest.java` or `server/ProtocolExecutorsTest.java` where the bridge is exercised.
- Verify request conversion, synchronous response conversion, every SSE event transition, tool call identifiers, stop reasons, usage mapping, unsupported-field rejection, error mapping, and audit visibility.

## Streaming, Cancellation, and Audit

- Java implementation: `provider/ServerSentEventObservable.java`, `provider/ProviderStreamObservable.java`, `server/StreamingResponseBridge.java`, both executors, `server/LlmProviderCallMonitor.java`, `provider/ProviderHttpCallMonitor.java`, and the `audit` package.
- Tests: `provider/ServerSentEventObservableTest.java`, `server/ProtocolExecutorsTest.java`, `server/LlmProviderCallMonitorTest.java`, and tests under `audit`.
- Treat downstream response commitment as a state transition. Trace cancellation, late provider signals, malformed SSE, partial streams, first-token timing, usage collection, and exactly-once monitor completion.

## Token Rate Limiting

- Contract: `documents/ai-llm-token-rate-limit-requirements.md`.
- Java implementation: the complete `limit` package, rate-limit classes under `model`, and token-rate-limit handlers plus usage extractors under `server`.
- Tests: matching `limit`, `model`, and `server` tests, especially admission, settlement, window rollover, distributed shard selection, missing usage, and concurrent requests.
- Separate local owner state, distributed coordination, admission, and settlement. Preserve the rule for every terminal request outcome rather than only successful non-streaming responses.

## JSON Path and Request Mutation

- Java implementation: the complete `jsonpath` package plus `server/LlmBodyExtractor.java`, `server/LlmBodyModifier.java`, `server/LlmRoutingData.java`, and their tests.
- Check path conflict behavior, variable scope, absent versus null values, mutation order, body-size limits, invalid JSON, and whether the original bytes or a rewritten document continue downstream.
- In C++, choose request-scoped pool-backed JSON values or streaming extraction where suitable; do not copy Java maps and lists by default.

## Scripts and Service Injection

- Documents: `documents/service.md`, `documents/service.d.ts`, `documents/ai-injector-lifecycle.md`, and `documents/fiber-net-gateway-index.md`.
- This surface reaches beyond `ploto-llm`. Trace the referenced Java gateway modules before declaring compatibility, and inspect `src/script` in C++ before designing adapters or value ownership.

## Useful Commands

List the live Java surface:

```bash
rg --files /home/dear/CLionProjects/ploto-gateway/ploto-llm/src | sort
rg -n "class|interface|record|enum" /home/dear/CLionProjects/ploto-gateway/ploto-llm/src/main/java
```

Find matching Java tests for a production class:

```bash
rg -n "ProductionClassName|observable behavior" /home/dear/CLionProjects/ploto-gateway/ploto-llm/src/test/java
```

Run selected Java tests from `/home/dear/CLionProjects/ploto-gateway` when reference behavior needs execution:

```bash
mvn -pl ploto-llm -am \
    -Dtest=TestClassOne,TestClassTwo \
    -Dsurefire.failIfNoSpecifiedTests=false test
```

Build and test the C++ slice with the actual target names from `apps/ai-server/CMakeLists.txt`; do not guess target names before that file exists. Then run the appropriate wider suite with `ctest --test-dir build --output-on-failure`.
