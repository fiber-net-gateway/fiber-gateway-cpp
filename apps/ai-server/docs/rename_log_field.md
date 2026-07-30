# Runtime 字段名对照表（新 audit_json -> 旧列名）

> runtime 产出 audit_json 时，按右列旧列名输出，ETL 同名直取。

| 当前 audit_json 路径 | 改为旧列名 | 备注                                   |
|---|---|--------------------------------------|
| `audit.id` | `request_id` |                                      |
| `identity.user` | `auth_user` |                                      |
| `identity.kid` | `auth_kid` |                                      |
| `identity.auth` | `auth` |                                      |
| `identity.auth_reason` | `authz_reason` |                                      |
| `routing.authorization` | `authz` |                                      |
| `routing.resolved_model_name` | `model` | 路由后模型名                               |
| `request.request_model_name` | `requested_model` | 原始模型名                                |
| `request.protocol` | `client_protocol` |                                      |
| `request.method` | `method` |                                      |
| `request.path` | `path` |                                      |
| `request.remote_addr` | `remote_addr` |                                      |
| `request.body_encoding` | `content_type` |                                      |
| `request.stream` | `stream` |                                      |
| `request.messages_count` | `message_count` |                                      |
| `request.tools_count` | `tool_count` |                                      |
| `request.body_bytes` | `request_body_bytes` |                                      |
| `request.body_sha256` | `request_body_hash` |                                      |
| `request.body` | `request_json` | 不套 rawBody，直接原文                      |
| `response.status` | `status` |                                      |
| `response.body_bytes` | `response_body_bytes` |                                      |
| `response.completed` 取反 | `client_aborted` | runtime 取反                           |
| `response.terminal_error` | `error_json` | 拼装见下                                 |
| `llm.output.content` | `response_json` | available=false 时空串                  |
| `llm.output.finish_reason` | `finish_reason` |                                      |
| `llm.output.tool_names` | `tool_names` |                                      |
| `usage` | `usage_json` | 结构见下                                 |
| `duration_us` | `duration_ms` | ÷1000 换毫秒                            |
| `provider_attempts[末条].status` | `upstream_status` | 取末次                                  |
| `provider_attempts[末条].latency_us` | `upstream_latency_ms` | 取末次，÷1000                            |
| `provider_attempts` | `attempts_json` | 整段 JSON 串                            |
| `rate_limit` | `rate_limit_json` | 整段 JSON 串                            |
| **缺失** | `user_agent` | ⚠️ runtime 从 HTTP header 补           |
| **缺失** | `host` | runtime 从 Host 头补                    |
| **缺失** | `real_ip` | runtime 从 request header X-Real-Ip 补 |
| `request.started_at_ms` | `kafka_ts` / `call_time` | ETL 派生                               |

## usage_json 内部字段对照

| 当前 usage 字段 | 改为旧字段名 | 转换 |
|---|---|---|
| `in_cache` + `in_nocache` | `promptTokens` | 求和 |
| `out` | `completionTokens` | |
| `total_tokens` | `total_tokens` | |
