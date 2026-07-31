# Java 配置 fixtures

这些输入以 `ploto-gateway` commit
`22c2bf543b96b52c0ccecd4ceb07d4911c502f45` 的
`JsonUtil.MAPPER.readValue(..., ProjectConf.class)` 为 oracle。

- `project-conf-full.json`：覆盖统一接入配置的主要字段和默认值；
- `project-conf-jackson-coercions.json`：覆盖未知字段、重复字段后值覆盖、primitive/string
  转型、null 和 custom Duration/DataSize codec。

期望的归一化结果由 `AccessConfigCodecTest.cpp` 逐字段断言。fixture 保留重复 JSON key，
不能经过会主动去重或排序 object 字段的格式化工具。

通用脚本语法不是本 fixture 集的兼容目标；condition/template 字段在这里仅作为原始
配置字符串保存。
