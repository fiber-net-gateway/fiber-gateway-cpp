# Access Server

`access-server` 是 Java
`../ploto-gateway/unified-access-server` 与其业务模块
`../ploto-gateway/ploto-unified-access` 的 C++23 迁移目标。实现将复用本仓库的
fiber runtime、HTTP、脚本、Nacos、CAT 和 Prometheus 能力，并以 Java 当前行为作为
兼容性基线。

## 当前状态

当前仅完成应用脚手架：

- CMake 已注册 `fiber_app_access_server`，产物名为 `access-server`；
- 已建立迁移范围、源码映射和分阶段验收清单；
- 尚未实现 HTTP listener、Nacos 配置订阅、Host/Path 路由、代理或监控；
- 当前二进制会明确报告迁移尚未完成并返回非零状态，不能用于部署。

首次迁移基线为 `ploto-gateway` commit
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45`。后续若 Java 基线发生变化，应先更新
[`docs/migration-plan.md`](docs/migration-plan.md) 中的基线和差异记录，再移植对应行为。

## 构建

```bash
cmake -S . -B build -DFIBER_BUILD_APPS=ON
cmake --build build --target fiber_app_access_server
```

产物位于：

```text
build/apps/access-server
```

## 运行

当前只提供脚手架状态说明：

```bash
./build/apps/access-server --help
```

不带参数启动会返回失败，防止尚未实现的程序被误认为可用服务。

## 目录

- `CMakeLists.txt`：应用目标和后续应用内静态库、测试的构建入口；
- `src/main.cpp`：当前占位入口，后续只保留进程级装配和生命周期管理；
- `docs/migration-plan.md`：Java 行为基线、目标模块划分、迁移顺序和验收门槛。

业务代码开始迁移后，按职责放入 `src/config/`、`src/routing/`、`src/proxy/`、
`src/runtime/` 和 `src/observability/`；对应测试放入 `tests/`，并在本目录的
`CMakeLists.txt` 中注册。

## 迁移原则

- Java 配置字段、默认值、Nacos data ID/group、路由优先级和错误结果属于外部兼容
  契约，未经明确决定不改名、不折叠；
- 配置更新先完整解析和校验，再以不可变快照发布；请求不能混用新旧配置；
- 热路径遵循本仓库的内存与异步约束，不按 Java 对象模型逐类机械翻译；
- 每一阶段先增加聚焦测试，再接入下一层运行时依赖；
- 可执行文件在具备最小端到端能力之前持续 fail closed。

详细范围与阶段见 [`docs/migration-plan.md`](docs/migration-plan.md)。
