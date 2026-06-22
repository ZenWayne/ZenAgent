# JSON 声明式 MCP 接入 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让作者在 workflow JSON 里用顶层 `mcp_servers` 块声明 MCP server，加载时自动连接、发现工具、按 `id.toolname` 命名空间注册进共享 `ToolRegistry`，agent 通过现有 `tools[]` 引用；连接失败时降级跳过而非中断。

**Architecture:** 在 `ToolRegistry::AttachMcpServer` 上加可选 `name_prefix` 做命名空间；把 `WorkflowLoader::Load` 解析后的收尾逻辑抽成共享 helper `FinalizeLoadedSpec`；新增协程 `WorkflowLoader::LoadAndAttach`，在"解析 → 连接 MCP → 降级裁剪 tools → 收尾校验/构建"之间插入 MCP 连接步骤。同步 `Load` 行为不变（遇到 `mcp_servers` 直接报错，引导改用 `LoadAndAttach`）。

**Tech Stack:** C++20 (asio coroutines, abseil status), protobuf3, nlohmann/json, Bazel (Bzlmod), GoogleTest。MCP 既有实现：`McpClient`(stdio)、`McpToolAdapter`、`McpClientPool`、`proto::McpServerSpec`。

## Global Constraints

- 命名空间分隔符为 `.`；server `id` 必须非空、唯一、**不含 `.`**。
- 命名空间注册：注册键/对 LLM 暴露的名 = `id.<remote>`；对远端 `tools/call` 仍用 **原始** remote 名。
- `include_tools` / `exclude_tools` 过滤按 **原始 remote 名** 匹配（加前缀之前）。
- 失败语义：声明的 server 连接失败 → 警告 + 跳过（降级），不中断 load。
- 引用裁剪：agent `tools[]` 中某项 registry 没有时——前缀属于"声明过但失败"的 server → 丢弃该项 + 警告；否则保留交给 `CheckReferences`（未声明前缀/拼写错 → 硬报错）。
- v1 对 JSON 声明的 server 一律 eager 连接；JSON 里的 `lazy_start` 被忽略并警告。
- 同步 `WorkflowLoader::Load` 行为不变；只为新协程 `LoadAndAttach` 增加 MCP 能力。
- proto 文件用 `strip_import_prefix = "/proto"`：import 写 `import "mcp_spec.proto";`（不带 `proto/`）。
- Bazel 首次拉依赖需代理参数：`bazel --host_jvm_args=-Dhttps.proxyHost=127.0.0.1 --host_jvm_args=-Dhttps.proxyPort=10808 ...`（宿主代理在 10809）。依赖已缓存后可省略。下文命令以普通 `bazel` 给出。
- 当前分支：`worktree-spec-json-mcp`（已隔离的 worktree）。

## File Structure

- `proto/workflow_spec.proto` — 新增 `WorkflowSpec.McpServerDecl` 与 `repeated McpServerDecl mcp_servers = 8`，import `mcp_spec.proto`。
- `proto/BUILD.bazel` — `workflow_spec_proto` 加 `deps = [":mcp_spec_proto"]`。
- `agentflow/tools/tool_registry.h` / `.cc` — `AttachMcpServer` 增加 `name_prefix` 形参 + 命名空间注册。
- `agentflow/workflow/workflow_loader.h` / `.cc` — 抽 `FinalizeLoadedSpec`；新增 `ParseMcpServers` + transport 解析；新增 `LoadAndAttach` 协程 + 降级裁剪；同步 `Load` 加 `mcp_servers` 守卫。
- `agentflow/workflow/BUILD.bazel` — `workflow_loader` 加 `@asio` 依赖。
- `tests/unit/tools/tool_registry_mcp_test.cc` — Task 1 命名空间测试。
- `tests/unit/tools/BUILD.bazel` — 新增 `fake_mcp_server` cc_library（供 loader 测试复用）。
- `tests/unit/workflow/workflow_loader_mcp_test.cc`（新建）+ `tests/unit/workflow/BUILD.bazel` 新 target — LoadAndAttach 单测（happy / id 校验 / 降级 / 错误）。
- `tests/integration/tools/loader_mcp_smoke_test.cc`（新建）+ BUILD target — 用真实 stdio echo server 端到端验证。

---

### Task 1: 命名空间化 AttachMcpServer

**Files:**
- Modify: `agentflow/tools/tool_registry.h:71`
- Modify: `agentflow/tools/tool_registry.cc:71-144`
- Test: `tests/unit/tools/tool_registry_mcp_test.cc`

**Interfaces:**
- Produces: `asio::awaitable<absl::Status> ToolRegistry::AttachMcpServer(proto::McpServerSpec spec, std::string name_prefix = "")` — 当 `name_prefix` 非空时，发现到的远端工具注册名为 `name_prefix + "." + remote`，但对远端调用仍用原始 remote 名；include/exclude 按原始名过滤。默认空 → 行为与今日完全一致。

- [ ] **Step 1: 写失败测试**

在 `tests/unit/tools/tool_registry_mcp_test.cc` 末尾（最后一个 `}  // namespace` 之前）追加：

```cpp
TEST(ToolRegistryMcpTest, AttachWithPrefixNamespacesNames) {
  asio::io_context io;
  auto h = std::make_shared<FakeFactoryHandle>();
  ToolRegistry reg(io, MakeFactory(h, [](FakeMcpClient& c) {
    c.on_list_tools = [] {
      return std::vector<ToolSchema>{MakeRemoteSchema("echo")};
    };
    c.on_call_tool = [](std::string_view name,
                        std::string_view /*args*/) -> absl::StatusOr<std::string> {
      // The remote must receive the RAW tool name, not the namespaced one.
      EXPECT_EQ(name, "echo");
      return std::string(R"({"ok":true})");
    };
  }));

  std::string invoke_out;
  bool attach_ok = false;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        auto st = co_await reg.AttachMcpServer(MakeSpec(), "fs");
        attach_ok = st.ok();
        CancelToken cancel;
        invoke_out = co_await reg.Invoke("fs.echo", "{}", cancel);
        co_return;
      },
      asio::detached);
  io.run();

  EXPECT_TRUE(attach_ok);
  EXPECT_TRUE(reg.Has("fs.echo"));
  EXPECT_FALSE(reg.Has("echo"));
  EXPECT_NE(invoke_out.find("ok"), std::string::npos);
}
```

若文件未包含 `<asio/detached.hpp>`，在顶部 include 区加上 `#include <asio/detached.hpp>`。

- [ ] **Step 2: 跑测试确认失败**

Run: `bazel test //tests/unit/tools:tool_registry_mcp_test --test_output=all`
Expected: 编译失败（`AttachMcpServer` 不接受第二个参数）或 `reg.Has("fs.echo")` 为 false。

- [ ] **Step 3: 改签名（头文件）**

`agentflow/tools/tool_registry.h:71` 把声明改为：

```cpp
  asio::awaitable<absl::Status> AttachMcpServer(
      proto::McpServerSpec spec, std::string name_prefix = "");
```

并在文件顶 include 区确认有 `#include <string>`（已有）。

- [ ] **Step 4: 改实现（命名空间注册）**

`agentflow/tools/tool_registry.cc` 把定义首行改为：

```cpp
asio::awaitable<absl::Status> ToolRegistry::AttachMcpServer(
    proto::McpServerSpec spec, std::string name_prefix) {
```

把注册循环（当前 116-137 行的 `for (auto& schema : *list) { ... }`）整体替换为：

```cpp
  for (auto& schema : *list) {
    // Filters match the RAW remote name (before namespacing).
    if (!include_set.empty() &&
        include_set.find(schema.name) == include_set.end()) {
      ++skipped_filtered;
      continue;
    }
    if (exclude_set.find(schema.name) != exclude_set.end()) {
      ++skipped_filtered;
      continue;
    }

    const std::string remote_name = schema.name;
    ToolSchema registered_schema = schema;  // copy; mutate the registry-facing name
    if (!name_prefix.empty()) {
      registered_schema.name = absl::StrCat(name_prefix, ".", remote_name);
    }
    auto adapter = std::make_shared<mcp::McpToolAdapter>(
        client, remote_name, registered_schema, timeout);
    if (TryRegisterIfAbsent(adapter)) {
      ++registered;
    } else {
      ++skipped_collision;
      std::fprintf(stderr,
                   "[ToolRegistry] skipping MCP tool '%s' from '%s' — name "
                   "already registered (local/earlier wins)\n",
                   registered_schema.name.c_str(),
                   spec.command_or_url().c_str());
    }
  }
```

确认文件顶部已 `#include <absl/strings/str_cat.h>`；若无则加上。

- [ ] **Step 5: 跑测试确认通过 + 回归**

Run: `bazel test //tests/unit/tools:tool_registry_mcp_test --test_output=errors`
Expected: PASS（含原有用例：默认空前缀行为不变）。

- [ ] **Step 6: 提交**

```bash
git add agentflow/tools/tool_registry.h agentflow/tools/tool_registry.cc tests/unit/tools/tool_registry_mcp_test.cc
git commit -m "feat(tools): AttachMcpServer optional name_prefix for MCP tool namespacing"
```

---

### Task 2: 抽出 FinalizeLoadedSpec（纯重构）

**Files:**
- Modify: `agentflow/workflow/workflow_loader.cc:463-574`
- Test: `tests/unit/workflow/workflow_loader_test.cc`（现有，作为回归护栏）

**Interfaces:**
- Produces: 匿名 namespace 内的 `absl::StatusOr<std::shared_ptr<Workflow>> FinalizeLoadedSpec(proto::WorkflowSpec spec, const ordered_json& root, const ToolRegistry& host_tools, size_t json_bytes, const Options& opts)` —— 执行 schema_version 归一、资源上限、引用校验、无环校验、模板校验、签名校验、proto_dynamic 状态池构建、`Workflow` 构建。供 `Load` 与（后续）`LoadAndAttach` 共用。

> 说明：这是**行为不变**的重构。`Load` 内"ParseRoot 之后"的所有逻辑搬进 `FinalizeLoadedSpec`，靠现有 `workflow_loader_test` 套件保证不回归。

- [ ] **Step 1: 先跑现有测试，确认绿**

Run: `bazel test //tests/unit/workflow:workflow_loader_test --test_output=errors`
Expected: PASS（重构前的基线）。

- [ ] **Step 2: 新增 FinalizeLoadedSpec helper**

在 `agentflow/workflow/workflow_loader.cc` 的匿名 namespace 末尾（`}  // namespace` 之前，紧接 `VerifySignature` 之后）插入：

```cpp
absl::StatusOr<std::shared_ptr<Workflow>> FinalizeLoadedSpec(
    proto::WorkflowSpec spec, const ordered_json& root,
    const ToolRegistry& host_tools, size_t json_bytes, const Options& opts) {
  // Forward-leniency: treat absent/zero schema_version as the current min.
  if (spec.schema_version() == 0) spec.set_schema_version(1);
  if (spec.schema_version() > kCurrentWorkflowSchemaVersion) {
    return absl::FailedPreconditionError(absl::StrCat(
        "schema_version ", spec.schema_version(),
        " > supported ", kCurrentWorkflowSchemaVersion));
  }

  if (auto s = CheckResourceLimits(spec, json_bytes, opts.max_json_bytes);
      !s.ok())
    return s;
  if (auto s = CheckReferences(spec, host_tools); !s.ok()) return s;
  if (auto s = CheckAcyclic(spec); !s.ok()) return s;
  if (auto s = CheckTemplates(spec); !s.ok()) return s;

  const bool has_signing = spec.has_signing() &&
                           (!spec.signing().algo().empty() ||
                            !spec.signing().signature().empty());
  if (has_signing) {
    if (opts.key_resolver != nullptr) {
      const std::string canon = CanonicalForm(root);
      if (auto s = VerifySignature(canon, spec, *opts.key_resolver); !s.ok()) {
        return s;
      }
    }
  } else if (opts.require_signed) {
    return absl::FailedPreconditionError("signature_required");
  }

  std::shared_ptr<google::protobuf::DescriptorPool> state_pool;
  if (spec.state().kind() == "proto_dynamic") {
    std::string desc_bytes;
    if (!spec.state().descriptor_set_b64().empty()) {
      if (!absl::Base64Unescape(spec.state().descriptor_set_b64(),
                                &desc_bytes)) {
        return absl::InvalidArgumentError("descriptor_set_b64 decode failed");
      }
    } else if (!spec.state().descriptor_set_path().empty()) {
      std::ifstream in(spec.state().descriptor_set_path(), std::ios::binary);
      if (!in) {
        return absl::NotFoundError(absl::StrCat(
            "descriptor_set_path not readable: ",
            spec.state().descriptor_set_path()));
      }
      std::stringstream ss;
      ss << in.rdbuf();
      desc_bytes = ss.str();
    } else {
      return absl::InvalidArgumentError(
          "proto_dynamic requires descriptor_set_path or descriptor_set_b64");
    }
    google::protobuf::FileDescriptorSet fds;
    if (!fds.ParseFromString(desc_bytes)) {
      return absl::InvalidArgumentError("descriptor_set parse failed");
    }
    state_pool = std::make_shared<google::protobuf::DescriptorPool>();
    for (const auto& fdp : fds.file()) {
      if (state_pool->BuildFile(fdp) == nullptr) {
        return absl::InvalidArgumentError(
            absl::StrCat("BuildFile failed for ", fdp.name()));
      }
    }
    if (spec.state().message_type().empty()) {
      return absl::InvalidArgumentError("proto_dynamic requires message_type");
    }
    if (!state_pool->FindMessageTypeByName(spec.state().message_type())) {
      return absl::InvalidArgumentError(absl::StrCat(
          "descriptor for ", spec.state().message_type(),
          " not found in supplied set"));
    }
  }

  auto workflow = std::make_shared<Workflow>(std::move(spec));
  if (state_pool) workflow->SetStatePool(std::move(state_pool));
  return workflow;
}
```

- [ ] **Step 3: 让 Load 调用 helper**

把 `WorkflowLoader::Load`（463 行起）的函数体替换为：

```cpp
absl::StatusOr<std::shared_ptr<Workflow>> WorkflowLoader::Load(
    std::string_view json_text,
    const ToolRegistry& host_tools,
    const Options& opts) {
  if (json_text.size() > opts.max_json_bytes) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "workflow json size ", json_text.size(),
        " > limit ", opts.max_json_bytes));
  }
  auto root = ordered_json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    return absl::InvalidArgumentError("malformed json");
  }

  proto::WorkflowSpec spec;
  if (auto s = ParseRoot(root, &spec); !s.ok()) return s;

  return FinalizeLoadedSpec(std::move(spec), root, host_tools,
                            json_text.size(), opts);
}
```

- [ ] **Step 4: 跑测试确认仍绿**

Run: `bazel test //tests/unit/workflow:workflow_loader_test --test_output=errors`
Expected: PASS（行为不变）。

- [ ] **Step 5: 提交**

```bash
git add agentflow/workflow/workflow_loader.cc
git commit -m "refactor(workflow): extract FinalizeLoadedSpec from WorkflowLoader::Load"
```

---

### Task 3: proto + 解析 mcp_servers + LoadAndAttach（happy path）

**Files:**
- Modify: `proto/workflow_spec.proto`
- Modify: `proto/BUILD.bazel:41-46`
- Modify: `agentflow/workflow/workflow_loader.h`
- Modify: `agentflow/workflow/workflow_loader.cc`
- Modify: `agentflow/workflow/BUILD.bazel:113-129`
- Create: `tests/unit/tools/BUILD.bazel`（新增 `fake_mcp_server` cc_library）
- Create: `tests/unit/workflow/workflow_loader_mcp_test.cc`
- Modify: `tests/unit/workflow/BUILD.bazel`

**Interfaces:**
- Consumes: `ToolRegistry::AttachMcpServer(spec, name_prefix)`（Task 1）、`FinalizeLoadedSpec(...)`（Task 2）。
- Produces:
  - proto `WorkflowSpec.McpServerDecl { string id = 1; McpServerSpec spec = 2; }` 与 `repeated McpServerDecl mcp_servers = 8;`
  - `asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>> WorkflowLoader::LoadAndAttach(std::string_view json_text, ToolRegistry& registry, const Options& opts)` 及无 opts 重载。
  - 同步 `Load` 在检测到 `spec.mcp_servers_size() > 0` 时返回 `InvalidArgumentError`，提示改用 `LoadAndAttach`。

- [ ] **Step 1: 写失败测试（先建测试基建）**

新建 `tests/unit/workflow/workflow_loader_mcp_test.cc`：

```cpp
// tests/unit/workflow/workflow_loader_mcp_test.cc
#include "agentflow/workflow/workflow_loader.h"

#include <memory>
#include <string>
#include <vector>

#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/tools/mcp_client_pool.h"
#include "agentflow/tools/tool.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"
#include "mcp_spec.pb.h"
#include "tests/unit/tools/fake_mcp_server.h"

namespace agentflow::workflow {
namespace {

using ::agentflow::testing::FakeMcpClient;

ToolSchema RemoteSchema(std::string name) {
  return ToolSchema{.name = std::move(name),
                    .description = "remote",
                    .params_json_schema = R"({"type":"object"})"};
}

// Factory that returns a FakeMcpClient configured by `configure`.
mcp::McpClientPool::ClientFactory MakeFactory(
    std::function<void(FakeMcpClient&)> configure) {
  return [configure = std::move(configure)](
             const proto::McpServerSpec& /*spec*/,
             asio::io_context& io) -> std::shared_ptr<mcp::IMcpClient> {
    auto c = std::make_shared<FakeMcpClient>(io);
    if (configure) configure(*c);
    return c;
  };
}

constexpr char kJsonWithMcp[] = R"({
  "schema_version": 1,
  "name": "wf",
  "version": "v1",
  "state": {"kind": "dynamic_json", "fields": {"user_query": {"type":"string"}}},
  "mcp_servers": [
    {"id": "fs", "transport": "stdio", "command_or_url": "/usr/bin/fake",
     "args": ["--root", "/data"]}
  ],
  "agents": {
    "chat": {"system_prompt": "hi", "tools": ["fs.echo"]}
  },
  "main": "chat"
})";

TEST(WorkflowLoaderMcpTest, LoadAndAttachRegistersAndResolvesNamespacedTool) {
  asio::io_context io;
  ToolRegistry reg(io, MakeFactory([](FakeMcpClient& c) {
    c.on_list_tools = [] {
      return std::vector<ToolSchema>{RemoteSchema("echo")};
    };
  }));

  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        result = co_await WorkflowLoader::LoadAndAttach(kJsonWithMcp, reg);
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(reg.Has("fs.echo"));
}

TEST(WorkflowLoaderMcpTest, DuplicateServerIdRejected) {
  asio::io_context io;
  ToolRegistry reg(io, MakeFactory(nullptr));
  constexpr char kDup[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[
      {"id":"x","transport":"stdio","command_or_url":"/a"},
      {"id":"x","transport":"stdio","command_or_url":"/b"}],
    "agents":{"c":{"system_prompt":"h","tools":[]}},"main":"c"})";

  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await WorkflowLoader::LoadAndAttach(kDup, reg);
    co_return; }, asio::detached);
  io.run();

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "duplicate"));
}

TEST(WorkflowLoaderMcpTest, DottedServerIdRejected) {
  asio::io_context io;
  ToolRegistry reg(io, MakeFactory(nullptr));
  constexpr char kDot[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[{"id":"a.b","transport":"stdio","command_or_url":"/a"}],
    "agents":{"c":{"system_prompt":"h","tools":[]}},"main":"c"})";

  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await WorkflowLoader::LoadAndAttach(kDot, reg);
    co_return; }, asio::detached);
  io.run();

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "."));
}

TEST(WorkflowLoaderMcpTest, SyncLoadRejectsMcpServers) {
  asio::io_context io;
  ToolRegistry host(io);
  auto result = WorkflowLoader::Load(kJsonWithMcp, host);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "LoadAndAttach"));
}

}  // namespace
}  // namespace agentflow::workflow
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bazel test //tests/unit/workflow:workflow_loader_mcp_test --test_output=all`
Expected: 失败 —— 目标不存在 / `LoadAndAttach` 未声明 / `mcp_servers` proto 字段缺失。

- [ ] **Step 3: 改 proto schema**

`proto/workflow_spec.proto`：在 `syntax`/`package` 之后加 import：

```proto
import "mcp_spec.proto";
```

在 `message WorkflowSpec { ... }` 内，`map<string, AgentDef> agents = 5;` 之后、`string main = 6;` 之前（或紧接 `signing` 之后均可，只要 tag=8 不冲突）加入：

```proto
  message McpServerDecl {
    string id          = 1;   // namespace prefix; non-empty, unique, no '.'
    McpServerSpec spec = 2;   // transport / command_or_url / args / include / exclude / timeout
  }
  repeated McpServerDecl mcp_servers = 8;
```

- [ ] **Step 4: 改 proto BUILD 依赖**

`proto/BUILD.bazel` 把 `workflow_spec_proto`（41-46 行）改为带 deps：

```python
proto_library(
    name = "workflow_spec_proto",
    srcs = ["workflow_spec.proto"],
    strip_import_prefix = "/proto",
    deps = [":mcp_spec_proto"],
    visibility = ["//visibility:public"],
)
```

（`agentflow_proto` 已聚合 `mcp_spec_cc_proto` 与 `workflow_spec_cc_proto`，C++ 侧无需再改。）

- [ ] **Step 5: 声明 LoadAndAttach（头文件）**

`agentflow/workflow/workflow_loader.h` 顶部 include 区加：

```cpp
#include <asio/awaitable.hpp>
```

在 `class WorkflowLoader` 内、`LoadFromFile` 重载之后加入：

```cpp
  // Like Load, but also connects any top-level `mcp_servers` declared in the
  // JSON, registering each server's tools into `registry` under the namespace
  // `<id>.<remote>`. Servers that fail to connect are skipped (degrade); agent
  // `tools[]` entries whose prefix names a skipped server are dropped. Requires
  // an MCP-aware ToolRegistry (io_context ctor). lazy_start in JSON is ignored.
  [[nodiscard]] static asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>>
  LoadAndAttach(std::string_view json_text, ToolRegistry& registry,
                const Options& opts);

  [[nodiscard]] static asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>>
  LoadAndAttach(std::string_view json_text, ToolRegistry& registry) {
    return LoadAndAttach(json_text, registry, Options{});
  }
```

- [ ] **Step 6: 实现解析 + 协程（.cc）**

`agentflow/workflow/workflow_loader.cc` 顶部 include 区补：

```cpp
#include <algorithm>
#include <unordered_set>
#include <asio/awaitable.hpp>
```

在匿名 namespace 内（`ParseSigning` 之后、`ParseRoot` 之前）加入 transport 解析与 mcp_servers 解析：

```cpp
absl::StatusOr<proto::McpServerSpec::Transport> ParseTransport(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  if (v == "stdio") return proto::McpServerSpec::STDIO;
  if (v == "http_sse" || v == "http" || v == "sse")
    return proto::McpServerSpec::HTTP_SSE;
  if (v == "websocket" || v == "ws") return proto::McpServerSpec::WEBSOCKET;
  if (v == "tcp") return proto::McpServerSpec::TCP;
  return absl::InvalidArgumentError(absl::StrCat("unknown transport '", v, "'"));
}

constexpr size_t kMaxMcpServers = 16;

absl::Status ParseMcpServers(const ordered_json& arr,
                             proto::WorkflowSpec* spec) {
  if (!arr.is_array()) {
    return absl::InvalidArgumentError("mcp_servers must be an array");
  }
  if (arr.size() > kMaxMcpServers) {
    return absl::ResourceExhaustedError(absl::StrCat(
        "mcp_servers count ", arr.size(), " > limit ", kMaxMcpServers));
  }
  std::unordered_set<std::string> seen;
  for (const auto& e : arr) {
    if (!e.is_object()) {
      return absl::InvalidArgumentError("mcp_servers entries must be objects");
    }
    auto* decl = spec->add_mcp_servers();

    auto idit = e.find("id");
    if (idit == e.end() || !idit->is_string() ||
        idit->get<std::string>().empty()) {
      return absl::InvalidArgumentError(
          "mcp_servers entry requires a non-empty string 'id'");
    }
    std::string id = idit->get<std::string>();
    if (id.find('.') != std::string::npos) {
      return absl::InvalidArgumentError(
          absl::StrCat("mcp_server id '", id, "' must not contain '.'"));
    }
    if (!seen.insert(id).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("duplicate mcp_server id '", id, "'"));
    }
    decl->set_id(id);

    auto* s = decl->mutable_spec();
    std::string transport = "stdio";
    if (auto t = e.find("transport"); t != e.end() && t->is_string()) {
      transport = t->get<std::string>();
    }
    auto tr = ParseTransport(transport);
    if (!tr.ok()) return tr.status();
    s->set_transport(*tr);

    if (auto c = e.find("command_or_url"); c != e.end() && c->is_string()) {
      s->set_command_or_url(c->get<std::string>());
    }
    if (auto a = e.find("args"); a != e.end() && a->is_array()) {
      for (const auto& x : *a)
        if (x.is_string()) s->add_args(x.get<std::string>());
    }
    if (auto inc = e.find("include_tools"); inc != e.end() && inc->is_array()) {
      for (const auto& x : *inc)
        if (x.is_string()) s->add_include_tools(x.get<std::string>());
    }
    if (auto exc = e.find("exclude_tools"); exc != e.end() && exc->is_array()) {
      for (const auto& x : *exc)
        if (x.is_string()) s->add_exclude_tools(x.get<std::string>());
    }
    if (auto to = e.find("call_timeout_ms");
        to != e.end() && to->is_number_integer()) {
      s->set_call_timeout_ms(to->get<int32_t>());
    }
    if (auto lz = e.find("lazy_start"); lz != e.end() && lz->is_boolean()) {
      s->set_lazy_start(lz->get<bool>());
    }
  }
  return absl::OkStatus();
}
```

在 `ParseRoot` 内，`agents` 解析块之后、`signing` 解析块之前，加入：

```cpp
  if (auto it = root.find("mcp_servers"); it != root.end()) {
    if (auto s = ParseMcpServers(*it, spec); !s.ok()) return s;
  }
```

确认顶部已 include `<cctype>`（`std::tolower`）；若无则加上。

- [ ] **Step 7: 同步 Load 加 mcp_servers 守卫**

在 `WorkflowLoader::Load` 内，`ParseRoot` 成功之后、`return FinalizeLoadedSpec(...)` 之前插入：

```cpp
  if (spec.mcp_servers_size() > 0) {
    return absl::InvalidArgumentError(
        "workflow declares mcp_servers; use WorkflowLoader::LoadAndAttach");
  }
```

- [ ] **Step 8: 实现 LoadAndAttach 协程**

在 `WorkflowLoader::LoadFromFile` 定义之后（文件末尾 `}` namespace 之前）加入：

```cpp
asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>>
WorkflowLoader::LoadAndAttach(std::string_view json_text,
                             ToolRegistry& registry, const Options& opts) {
  if (json_text.size() > opts.max_json_bytes) {
    co_return absl::ResourceExhaustedError(absl::StrCat(
        "workflow json size ", json_text.size(),
        " > limit ", opts.max_json_bytes));
  }
  auto root = ordered_json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    co_return absl::InvalidArgumentError("malformed json");
  }

  proto::WorkflowSpec spec;
  if (auto s = ParseRoot(root, &spec); !s.ok()) co_return s;

  // Connect declared MCP servers (eager). Failures degrade: warn + skip.
  for (const auto& decl : spec.mcp_servers()) {
    proto::McpServerSpec server = decl.spec();
    if (server.lazy_start()) {
      std::fprintf(stderr,
                   "[WorkflowLoader] mcp_server '%s': lazy_start ignored "
                   "(eager connect required for JSON-declared servers)\n",
                   decl.id().c_str());
      server.set_lazy_start(false);
    }
    absl::Status st = co_await registry.AttachMcpServer(server, decl.id());
    if (!st.ok()) {
      std::fprintf(stderr,
                   "[WorkflowLoader] mcp_server '%s' attach failed: %s — "
                   "skipping (degrade)\n",
                   decl.id().c_str(), std::string(st.message()).c_str());
    }
  }

  co_return FinalizeLoadedSpec(std::move(spec), root, registry,
                               json_text.size(), opts);
}
```

> 注意：本任务的 happy path 测试里所有 server 都连接成功，因此暂不做"降级裁剪 tools"。裁剪逻辑在 Task 4 用 TDD 加入。

- [ ] **Step 9: 新增 fake_mcp_server cc_library**

`tests/unit/tools/BUILD.bazel` 末尾追加：

```python
cc_library(
    name = "fake_mcp_server",
    testonly = True,
    hdrs = ["fake_mcp_server.h"],
    deps = [
        "//agentflow/core",
        "//agentflow/tools",
        "@abseil-cpp//absl/status",
        "@abseil-cpp//absl/status:statusor",
        "@asio",
    ],
    visibility = ["//visibility:public"],
)
```

- [ ] **Step 10: 新增 loader-mcp 测试 target**

`tests/unit/workflow/BUILD.bazel` 追加：

```python
cc_test(
    name = "workflow_loader_mcp_test",
    size = "small",
    srcs = ["workflow_loader_mcp_test.cc"],
    deps = [
        "//agentflow/tools",
        "//agentflow/workflow:workflow_loader",
        "//proto:agentflow_proto",
        "//tests/unit/tools:fake_mcp_server",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 11: 给 workflow_loader 加 @asio 依赖**

`agentflow/workflow/BUILD.bazel` 的 `workflow_loader` target deps 列表里加一行 `"@asio",`（按字母序放在 `@abseil-cpp//absl/strings` 之后）。

- [ ] **Step 12: 跑测试确认通过**

Run: `bazel test //tests/unit/workflow:workflow_loader_mcp_test //tests/unit/workflow:workflow_loader_test --test_output=errors`
Expected: PASS（4 个新用例通过，原 loader 测试仍绿）。

- [ ] **Step 13: 提交**

```bash
git add proto/workflow_spec.proto proto/BUILD.bazel \
        agentflow/workflow/workflow_loader.h agentflow/workflow/workflow_loader.cc \
        agentflow/workflow/BUILD.bazel \
        tests/unit/tools/BUILD.bazel \
        tests/unit/workflow/workflow_loader_mcp_test.cc tests/unit/workflow/BUILD.bazel
git commit -m "feat(workflow): LoadAndAttach + mcp_servers JSON schema (happy path)"
```

---

### Task 4: 降级裁剪与错误语义

**Files:**
- Modify: `agentflow/workflow/workflow_loader.cc`（`LoadAndAttach` 加裁剪逻辑）
- Test: `tests/unit/workflow/workflow_loader_mcp_test.cc`

**Interfaces:**
- Consumes: `LoadAndAttach`（Task 3）、`FakeMcpClient::on_connect`（可注入连接失败）。
- Produces: `LoadAndAttach` 在某 server 连接失败时，把 agent `tools[]` 中前缀属于该 server 的项丢弃（warn），其余交给 `CheckReferences`；前缀未声明的项仍硬报错。

- [ ] **Step 1: 写失败测试**

在 `tests/unit/workflow/workflow_loader_mcp_test.cc` 的匿名 namespace 内追加三个用例：

```cpp
TEST(WorkflowLoaderMcpTest, FailedServerDropsItsToolsAndBuilds) {
  asio::io_context io;
  ToolRegistry reg(io, MakeFactory([](FakeMcpClient& c) {
    c.on_connect = [] { return absl::UnavailableError("boom"); };
  }));
  // Agent references fs.echo; server fs fails → tool dropped, workflow builds.
  constexpr char kJson[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[{"id":"fs","transport":"stdio","command_or_url":"/x"}],
    "agents":{"chat":{"system_prompt":"h","tools":["fs.echo"]}},"main":"chat"})";

  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await WorkflowLoader::LoadAndAttach(kJson, reg);
    co_return; }, asio::detached);
  io.run();

  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_FALSE(reg.Has("fs.echo"));
}

TEST(WorkflowLoaderMcpTest, UndeclaredPrefixIsHardError) {
  asio::io_context io;
  ToolRegistry reg(io, MakeFactory(nullptr));
  // 'ghost.echo' references a server that was never declared.
  constexpr char kJson[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[],
    "agents":{"chat":{"system_prompt":"h","tools":["ghost.echo"]}},"main":"chat"})";

  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await WorkflowLoader::LoadAndAttach(kJson, reg);
    co_return; }, asio::detached);
  io.run();

  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(absl::StrContains(result.status().message(), "unknown tool"));
}

TEST(WorkflowLoaderMcpTest, LazyStartIgnoredStillRegisters) {
  asio::io_context io;
  ToolRegistry reg(io, MakeFactory([](FakeMcpClient& c) {
    c.on_list_tools = [] {
      return std::vector<ToolSchema>{RemoteSchema("echo")};
    };
  }));
  constexpr char kJson[] = R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{}},
    "mcp_servers":[{"id":"fs","transport":"stdio","command_or_url":"/x",
                    "lazy_start":true}],
    "agents":{"chat":{"system_prompt":"h","tools":["fs.echo"]}},"main":"chat"})";

  absl::StatusOr<std::shared_ptr<Workflow>> result;
  asio::co_spawn(io, [&]() -> asio::awaitable<void> {
    result = co_await WorkflowLoader::LoadAndAttach(kJson, reg);
    co_return; }, asio::detached);
  io.run();

  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(reg.Has("fs.echo"));  // eager connect happened despite lazy_start
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `bazel test //tests/unit/workflow:workflow_loader_mcp_test --test_output=all`
Expected: `FailedServerDropsItsToolsAndBuilds` 失败（当前无裁剪 → `CheckReferences` 报 "unknown tool 'fs.echo'"）。`LazyStartIgnoredStillRegisters` 应已通过（Task 3 已忽略 lazy_start）；`UndeclaredPrefixIsHardError` 应已通过。

- [ ] **Step 3: 加降级裁剪逻辑**

在 `agentflow/workflow/workflow_loader.cc` 的 `LoadAndAttach` 中，把连接循环改为收集失败的 server id，并在连接循环之后、`FinalizeLoadedSpec` 之前插入裁剪。即把连接循环替换为：

```cpp
  std::unordered_set<std::string> skipped_ids;
  for (const auto& decl : spec.mcp_servers()) {
    proto::McpServerSpec server = decl.spec();
    if (server.lazy_start()) {
      std::fprintf(stderr,
                   "[WorkflowLoader] mcp_server '%s': lazy_start ignored "
                   "(eager connect required for JSON-declared servers)\n",
                   decl.id().c_str());
      server.set_lazy_start(false);
    }
    absl::Status st = co_await registry.AttachMcpServer(server, decl.id());
    if (!st.ok()) {
      std::fprintf(stderr,
                   "[WorkflowLoader] mcp_server '%s' attach failed: %s — "
                   "skipping (degrade)\n",
                   decl.id().c_str(), std::string(st.message()).c_str());
      skipped_ids.insert(decl.id());
    }
  }

  // Degrade-drop: remove agent tool refs whose prefix names a skipped server.
  // Everything else is left for CheckReferences (undeclared prefix → error).
  if (!skipped_ids.empty()) {
    for (auto& [agent_name, def] : *spec.mutable_agents()) {
      google::protobuf::RepeatedPtrField<std::string> kept;
      for (const auto& t : def.tools()) {
        if (!registry.Has(t)) {
          auto dot = t.find('.');
          if (dot != std::string::npos) {
            std::string prefix = t.substr(0, dot);
            if (skipped_ids.count(prefix)) {
              std::fprintf(stderr,
                           "[WorkflowLoader] dropping tool '%s' from agent "
                           "'%s' — server '%s' unavailable\n",
                           t.c_str(), agent_name.c_str(), prefix.c_str());
              continue;  // drop
            }
          }
        }
        *kept.Add() = t;
      }
      *def.mutable_tools() = std::move(kept);
    }
  }
```

确认顶部已 include `<google/protobuf/repeated_ptr_field.h>`（多数 protobuf 版本经生成头间接引入；若编译报缺失则显式加 `#include <google/protobuf/repeated_field.h>`）。

- [ ] **Step 4: 跑测试确认全绿**

Run: `bazel test //tests/unit/workflow:workflow_loader_mcp_test --test_output=errors`
Expected: PASS（7 个用例）。

- [ ] **Step 5: 提交**

```bash
git add agentflow/workflow/workflow_loader.cc tests/unit/workflow/workflow_loader_mcp_test.cc
git commit -m "feat(workflow): degrade-drop tools of failed MCP servers in LoadAndAttach"
```

---

### Task 5: 端到端集成测试（真实 stdio echo server）

**Files:**
- Create: `tests/integration/tools/loader_mcp_smoke_test.cc`
- Modify: `tests/integration/tools/BUILD.bazel`
- 复用: `tests/integration/tools/echo_mcp_server.py`（已存在，工具名为 `echo`）

**Interfaces:**
- Consumes: `WorkflowLoader::LoadAndAttach`、真实 `McpClient` over stdio、`echo_mcp_server.py`。
- Produces: 证明 JSON→连接→命名空间注册→引用解析 在真实子进程下端到端可用。

- [ ] **Step 1: 写测试**

新建 `tests/integration/tools/loader_mcp_smoke_test.cc`：

```cpp
// tests/integration/tools/loader_mcp_smoke_test.cc
//
// End-to-end: a workflow JSON declares the stdio echo MCP server; LoadAndAttach
// connects it, namespaces its tool as "echo.echo", and an agent referencing it
// resolves cleanly. Exercises the real McpClient + child process path.

#include <cstdlib>
#include <memory>
#include <string>
#include <sys/stat.h>

#include <absl/status/statusor.h>
#include <absl/strings/match.h>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"
#include "agentflow/workflow/workflow_loader.h"

namespace agentflow {
namespace {

std::string EchoServerPath() {
  const char* srcdir = std::getenv("TEST_SRCDIR");
  if (srcdir != nullptr) {
    std::string candidate = std::string(srcdir) +
        "/_main/tests/integration/tools/echo_mcp_server.py";
    struct stat st;
    if (::stat(candidate.c_str(), &st) == 0) return candidate;
  }
  return "tests/integration/tools/echo_mcp_server.py";
}

TEST(LoaderMcpSmokeTest, LoadAndAttachStdioEcho) {
  asio::io_context io;
  ToolRegistry reg(io);

  const std::string json = std::string(R"({
    "schema_version":1,"name":"w","version":"v1",
    "state":{"kind":"dynamic_json","fields":{"user_query":{"type":"string"}}},
    "mcp_servers":[{"id":"echo","transport":"stdio",
                    "command_or_url":"/usr/bin/python3","args":[")") +
      EchoServerPath() + R"("]}],
    "agents":{"chat":{"system_prompt":"hi","tools":["echo.echo"]}},
    "main":"chat"})";

  absl::StatusOr<std::shared_ptr<workflow::Workflow>> result;
  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        result = co_await workflow::WorkflowLoader::LoadAndAttach(json, reg);
        reg.ShutdownMcp();
        co_return;
      },
      asio::detached);
  io.run();

  ASSERT_TRUE(result.ok()) << result.status().message();
  EXPECT_TRUE(reg.Has("echo.echo"));
}

}  // namespace
}  // namespace agentflow
```

- [ ] **Step 2: 加 BUILD target**

`tests/integration/tools/BUILD.bazel` 追加：

```python
cc_test(
    name = "loader_mcp_smoke_test",
    size = "medium",
    srcs = ["loader_mcp_smoke_test.cc"],
    data = ["echo_mcp_server.py"],
    deps = [
        "//agentflow/tools",
        "//agentflow/workflow:workflow_loader",
        "@abseil-cpp//absl/status:statusor",
        "@abseil-cpp//absl/strings",
        "@asio",
        "@googletest//:gtest",
        "@googletest//:gtest_main",
    ],
)
```

- [ ] **Step 3: 跑集成测试**

Run: `bazel test //tests/integration/tools:loader_mcp_smoke_test --test_output=all`
Expected: PASS —— `result.ok()` 为真，`reg.Has("echo.echo")` 为真。（需要系统 `/usr/bin/python3`。）

- [ ] **Step 4: 全量回归**

Run: `bazel test //tests/unit/tools:all //tests/unit/workflow:all //tests/integration/tools:all --test_output=errors`
Expected: 全 PASS。

- [ ] **Step 5: 提交**

```bash
git add tests/integration/tools/loader_mcp_smoke_test.cc tests/integration/tools/BUILD.bazel
git commit -m "test(integration): end-to-end LoadAndAttach over stdio echo MCP server"
```

---

## 自检（Self-Review）

**Spec 覆盖：**
- 顶层 `mcp_servers` 声明 + agent `tools[]` 引用 → Task 3（proto + 解析 + LoadAndAttach）。✅
- `id.toolname` 命名空间 → Task 1（registry 前缀）+ Task 3（解析）。✅
- 连接失败降级跳过 → Task 4。✅
- 声明过但失败的前缀丢工具+警告 / 未声明前缀硬报错 → Task 4（两个用例）。✅
- v1 忽略 `lazy_start` + 警告 → Task 3 实现，Task 4 `LazyStartIgnoredStillRegisters` 验证。✅
- include/exclude 按原始名 → Task 1（过滤在加前缀前）+ 现有 registry 测试覆盖。✅
- 同步 `Load` 不变 + mcp_servers 守卫 → Task 2（重构不变）+ Task 3（守卫 + `SyncLoadRejectsMcpServers`）。✅
- 生命周期 `ShutdownMcp` → Task 5 集成测试中调用演示。✅
- 受影响文件清单（proto / tool_registry / workflow_loader / 测试）→ 全部有对应 Task。✅

**占位符扫描：** 无 TBD/TODO；每个代码步骤含完整代码。✅

**类型一致性：** `LoadAndAttach` 签名（`std::string_view`, `ToolRegistry&`, `const Options&`）在 .h/.cc/测试中一致；`AttachMcpServer(spec, name_prefix)` 第二参 `std::string` 默认空，在 Task 1 与 Task 3/4 调用一致；`ParseMcpServers` / `ParseTransport` / `FinalizeLoadedSpec` 签名在声明与调用处一致。✅

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-06-22-json-declarative-mcp.md`.
