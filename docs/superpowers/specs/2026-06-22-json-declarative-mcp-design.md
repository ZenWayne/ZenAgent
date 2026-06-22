# JSON 声明式 MCP 接入设计（AgentFlow）

- **日期**: 2026-06-22
- **状态**: 设计已确认，待实现
- **范围**: 让作者在 workflow / agent 的 JSON 配置里直接声明 MCP server，加载时自动连接、发现工具并注册进共享 `ToolRegistry`，agent 通过 `tools[]` 按命名空间名引用。

## 1. 背景与动机

### 现状

AgentFlow 的 JSON 配置目标结构是 `proto/workflow_spec.proto` 里的 `WorkflowSpec`：

- 顶层有 `agents` map，每个 `AgentDef` 有 `tools: [...]`，**只是工具名字**。
- 加载器 `agentflow/workflow/workflow_loader.cc` 解析 JSON 后会校验：每个 `tools[]` 名字必须**已存在于**传入的 host `ToolRegistry`，否则报 `references unknown tool`（见 `workflow_loader.cc:284`）。
- MCP server 本身由 `proto/mcp_spec.proto` 的 `McpServerSpec` 描述（`transport` = STDIO/HTTP_SSE/WEBSOCKET/TCP、`command_or_url`、`args`、`include_tools`/`exclude_tools`、`call_timeout_ms`、`lazy_start`）。
- 注册 MCP 工具的入口是协程 `ToolRegistry::AttachMcpServer(McpServerSpec)`：连接 server → `tools/list` 发现工具 → 每个包成 `McpToolAdapter` 注册进 registry（`tool_registry.cc:71`）。

### Gap

`WorkflowSpec`（即 JSON schema）**没有任何 MCP server 字段**。今天要让 agent 用上 MCP 工具，必须在加载 JSON **之前**用 C++ 代码调 `AttachMcpServer`，JSON 里只能引用已注册的工具名。无法做到"纯 JSON 声明、自动接入"。

### 目标

在 JSON 里声明 MCP server，加载时自动：连接 → 发现 → 命名空间化注册 → 供 agent 按名引用。连接失败时降级而非中断整个 workflow。

## 2. 关键设计决策（已确认）

1. **声明位置**：顶层 `mcp_servers` 声明"连哪些 server"；每个 agent 用现有 `tools[]` 按名字挑子集。不给 `AgentDef` 加新字段。
2. **工具命名**：按 server 前缀命名空间，注册名为 `id.toolname`（如 `fs.read_file`）。`.` 为分隔符，因此 server `id` 不能含 `.`。
3. **失败语义**：加载时某 server 连不上 / 握手失败 → **降级**：警告 + 跳过该 server，继续加载，不中断 workflow。
4. **引用校验**：agent `tools[]` 引用一个拿不到的工具时：
   - 若其前缀对应一个**声明过但连接失败**的 server → 从该 agent 工具表**丢弃该工具 + 警告**；
   - 若前缀**未声明**（也不是原生工具）→ **硬报错**（`InvalidArgumentError`），接住拼写/漏配错误。

## 3. 架构总览 / 数据流

```
workflow.json
  ├─ mcp_servers: [ {id, transport, command_or_url, args, ...} ]   ← 新增顶层块
  └─ agents: { name: { tools: ["fs.read_file", "get_time", ...] } }
        │
        ▼
WorkflowLoader::LoadAndAttach(json, registry&, opts)   ← 新增协程入口
  1. 解析 JSON → WorkflowSpec（含 mcp_servers）
  2. 逐个 co_await registry.AttachMcpServer(spec, /*prefix=*/id)
        ├─ 成功 → 工具以 "id.toolname" 注册进共享 registry
        └─ 失败 → 警告 + 记入 skipped_server_ids（降级）
  3. 引用校验（带降级规则，见 §6）
  4. 构建 Workflow
        │
        ▼
WorkflowRunner → AgentNode（拿共享 registry，按 tools[] 名字挑子集）
```

同步 `Load` 保持原样（无 MCP / 工具预注册的场景行为完全不变）。新增协程 `LoadAndAttach` 负责"解析 → 连接 → 校验 → 构建"。

## 4. JSON Schema 变更（`proto/workflow_spec.proto`）

顶层新增 `mcp_servers`，复用现有 `McpServerSpec`，外面包一层带 `id` 的声明：

```protobuf
import "proto/mcp_spec.proto";

message WorkflowSpec {
  // ... 现有字段（schema_version / name / version / state / agents / main / signing）...

  message McpServerDecl {
    string id          = 1;   // 命名空间前缀；需唯一、非空、不含 '.'
    McpServerSpec spec = 2;   // transport / command_or_url / args / include_tools / exclude_tools / call_timeout_ms
  }
  repeated McpServerDecl mcp_servers = 8;   // 新字段（tag 8，避开现有 1..7）
}
```

作者编写的 JSON 示例：

```json
{
  "schema_version": 1,
  "name": "demo",
  "version": "v1",
  "state": { "kind": "dynamic_json", "fields": {} },
  "mcp_servers": [
    {
      "id": "fs",
      "transport": "stdio",
      "command_or_url": "/usr/bin/npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/data"],
      "include_tools": ["read_file", "list_directory"]
    }
  ],
  "agents": {
    "assistant": {
      "system_prompt": "You are helpful.",
      "model": { "max_output_tokens": 512 },
      "tools": ["fs.read_file", "fs.list_directory", "get_time"]
    }
  },
  "main": "assistant"
}
```

JSON 中 `transport` 接受字符串枚举：`"stdio"` / `"http_sse"` / `"websocket"` / `"tcp"`（大小写不敏感，映射到 `McpServerSpec::Transport`）。`include_tools` / `exclude_tools` 按**原始远端工具名**填写（不带前缀）。

## 5. 命名空间实现（`agentflow/tools/tool_registry.{h,cc}`）

给 `AttachMcpServer` 增加一个可选前缀参数（默认空 → 现有调用方零改动）：

```cpp
asio::awaitable<absl::Status> AttachMcpServer(
    proto::McpServerSpec spec, std::string_view name_prefix = "");
```

注册循环（`tool_registry.cc:117-128` 附近）调整：

- 远端调用名仍用原始 `schema.name`（`McpToolAdapter` 第二个参数 `remote_tool_name` 保持原始名）。
- 注册键改为：`name_prefix.empty() ? schema.name : absl::StrCat(name_prefix, ".", schema.name)`。做法：拷一份 `ToolSchema`，把 `.name` 改成带前缀的，作为 `McpToolAdapter` 的 `cached_schema` 传入；`McpToolAdapter::Schema().name`（registry 的键）即带前缀，而 `Invoke` 仍按原始 `remote_tool_name` 调 `client->CallTool`。
- `include_tools` / `exclude_tools` 过滤继续按**原始** `schema.name` 匹配（在加前缀之前）。
- 冲突仍走现有 `TryRegisterIfAbsent`（skip-on-collision）。命名空间已基本消除跨 server 冲突；同 server 内若远端有重名（不应发生）维持跳过语义。

## 6. 加载 / 连接 / 失败处理（`agentflow/workflow/workflow_loader.{h,cc}`）

新增协程静态方法（与现有同步 `Load` 并存）：

```cpp
[[nodiscard]] static asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>>
LoadAndAttach(std::string_view json, ToolRegistry& registry, const Options& opts);
```

要求传入的 `registry` 是 **MCP-aware** 的（用带 `asio::io_context&` 的 ctor 构造）；`AttachMcpServer` 用该 registry 内部的 `McpClientPool`，无需额外传 io_context。

流程：

1. **解析**：JSON → `WorkflowSpec`（复用现有解析逻辑；新增 `mcp_servers` 数组解析）。`id` 校验：非空、唯一、不含 `.`，否则 `InvalidArgumentError`。
2. **连接**：对每个 `decl`，`co_await registry.AttachMcpServer(decl.spec(), decl.id())`：
   - 返回 OK → 工具以 `id.tool` 注册。
   - 返回非 OK → `LOG(WARNING)` 记录 server `id` 与错误，把 `id` 加入本地 `skipped_server_ids`（**降级**，不中断）。
3. **引用校验（降级规则）**：构建已声明 server id 集合 `declared_ids`。对每个 agent 的每个 `tools[]` 名字 `t`：
   - `registry.Has(t)` → 保留。
   - 否则取前缀 `p`（`t` 中首个 `.` 之前；无 `.` 则 `p` 为空/视为原生名）：
     - `p ∈ skipped_server_ids` → 从该 agent 的工具列表**移除 `t`** + `LOG(WARNING)`。
     - 否则（`p` 不在 `declared_ids`，且 `t` 非原生工具）→ 返回 `InvalidArgumentError("agent '...' references unknown tool 't'")`。
4. **构建** `Workflow`（复用现有构建路径，传入经降级裁剪后的 spec）。

为减少重复，把现有 `Load` 内部"解析 + 校验 + 构建"拆出可复用的 helper（解析、引用校验、构建分离），让 `LoadAndAttach` 在解析与校验之间插入连接步骤。

### lazy_start 取舍（v1 范围外）

命名空间化和加载时引用校验都依赖加载期的 `tools/list`，因此 v1 对 JSON 声明的 server **一律 eager 连接 + 列举工具**。JSON 中出现的 `lazy_start` 在 v1 **忽略并警告**——否则加载时拿不到工具名，既无法命名空间化也无法校验引用。这是有意的 YAGNI 取舍；后续若要支持 lazy，需让作者在 JSON 里显式列出工具名与 schema。

## 7. 生命周期 & 调用方

- 调用方（workflow runner / JNI 桥 / examples）从 `Load` 切到 `co_await LoadAndAttach(...)`，并用 MCP-aware ctor 构造 `ToolRegistry`。
- workflow 结束时由 registry 持有方调用既有的 `registry.ShutdownMcp()`，关闭全部 MCP client（每个 client 持有 detached ReadLoop，需显式关闭）。

## 8. 测试计划

**测试一律用真实 MCP server，不用进程内 `FakeMcpClient`。** 复用既有的 `tests/integration/tools/echo_mcp_server.py`（真实 stdio MCP server，工具名 `echo`，入参 `text`）。曾评估用 GopherSecurity/gopher-mcp C++ SDK 自建 server，但它硬依赖 libevent+OpenSSL+fmt+yaml-cpp（无 client-only/无-libevent 开关）且本机 configure 失败，与"不膨胀依赖"冲突，故放弃。

纯解析 / 校验单测（不连接、不起子进程）：

- `id` 校验：重复 `id`、含 `.` 的 `id` 报错。
- 同步 `Load` 遇 `mcp_servers` → 报错引导用 `LoadAndAttach`。
- 未声明前缀 → 硬报错（`LoadAndAttach` 空 server 列表 + agent 引用 `ghost.echo`）。
- 降级：server 指向不存在命令 → 连接失败 → 引用其工具的 agent 丢工具 + 警告，workflow 仍构建成功。

集成测试（真实 echo server 子进程，需 `python3`）：

- 命名空间：`AttachMcpServer(spec, "remote")` → 注册名 `remote.echo`；`Invoke` 仍按原始名 `echo` 调用远端；`include/exclude` 按原始名过滤。
- 端到端 `LoadAndAttach`（JSON 声明 echo server）→ `registry.Has("echo.echo")`、workflow 构建成功、`ShutdownMcp` 收尾。
- `lazy_start` 在 JSON 中被忽略但仍 eager 注册。

## 9. 范围之外（YAGNI）

- JSON 路径的 `lazy_start`（v1 忽略并警告）。
- 每个 agent 独立 registry（仍共享一个）。
- MCP 的 prompts / resources（v1 只接 tools）。
- HTTP_SSE / WEBSOCKET / TCP transport 的实测（沿用 `McpClient` 现状：P3 仅 stdio 可用，其余返回 Unimplemented；schema 已预留，按现状透传）。

## 10. 受影响文件清单

- `proto/workflow_spec.proto` — 新增 `McpServerDecl` 与 `mcp_servers` 字段；import `mcp_spec.proto`。
- `agentflow/tools/tool_registry.h` / `.cc` — `AttachMcpServer` 增加 `name_prefix` 参数及命名空间注册逻辑。
- `agentflow/workflow/workflow_loader.h` / `.cc` — 新增 `LoadAndAttach` 协程；拆分可复用 helper；`mcp_servers` 解析、`id` 校验、降级引用校验。
- `tests/unit/workflow/workflow_loader_mcp_test.cc` — 纯解析/校验/降级单测（不起子进程）。
- `tests/integration/tools/mcp_namespace_smoke_test.cc` + `loader_mcp_smoke_test.cc` — 真实 echo server 集成测试。
- 调用方（runner / JNI / examples）— 切到 `LoadAndAttach` + MCP-aware registry（按需）。
