# C++ Agent Framework 设计文档

**日期**: 2026-04-27
**主题**: 基于 LiteRT-LM 与 gopher-mcp 的端侧 C++ Agent 框架（含 Kotlin DSL）
**状态**: 待审核
**项目代号**: agentflow

---

## 1. 项目概述

### 1.1 目标

构建一个**端侧/嵌入式**的 C++ Agent 框架，提供"从推理到 agent 全栈覆盖"的能力：

- 推理后端：[LiteRT-LM](https://github.com/google-ai-edge/LiteRT-LM)（Google 端侧 LLM 推理引擎，C ABI）
- 工具协议：[gopher-mcp](https://github.com/GopherSecurity/gopher-mcp)（C++ 实现的 MCP client/server）
- Workflow 引擎：DAG + 显式 activation_group，参考 [microsoft/autogen#6711](https://github.com/microsoft/autogen/pull/6711) 的循环边分组机制
- Multi-agent：Agent 默认作为 graph node，复杂动态调度封装为 Team 节点

### 1.2 部署形态

- **主战场**: Android（Kotlin DSL + JNI binding）
- **次要平台**: 嵌入式 Linux ARM64（树莓派、Jetson 等，纯 C++ API）
- **不支持**: iOS、桌面、服务器（v1 范围外）

### 1.3 交付形态

- **C++ 库**: `libagentflow.so` 是 source-of-truth API；公开头文件可被嵌入式 Linux 项目直接使用
- **Kotlin DSL**: Android AAR，包装 C++ API，提供声明式 workflow/agent/tool DSL；Kotlin 不持有业务逻辑
- **Native tool 注册**: 用户可在 Kotlin 端注册 lambda 作为 in-process tool（通过 callback 桥）

### 1.4 核心需求

| # | 需求 | 优先级 |
|---|---|---|
| 1 | 端侧本地 LLM 推理（通过 LiteRT-LM） | P0 |
| 2 | Tool calling（LLM 输出 tool_call → 框架执行 → 喂回 LLM） | P0 |
| 3 | MCP client（接外部 MCP server 作为 tool 来源） | P0 |
| 4 | In-process native tool（C++ / Kotlin lambda 直接注册） | P0 |
| 5 | DAG-based workflow，支持循环（用 activation_group 分组） | P0 |
| 6 | Token-level streaming + 协作式 cancel | P0 |
| 7 | Multi-agent（agent=node 默认 + TeamNode 动态调度） | P0 |
| 8 | Checkpoint hook（不自动 resume） | P1 |
| 9 | Trace event API（接 Perfetto / OpenTelemetry） | P1 |
| 10 | Constrained decoding（依赖 LiteRT-LM 原生支持） | P1 |

---

## 2. 设计决策汇总

下表是 brainstorming 阶段敲定的所有架构决策，作为后续实现的约束：

| # | 维度 | 选择 | 理由 |
|---|---|---|---|
| D1 | 目标场景 | 纯端侧/嵌入式 | 用户明确选定 |
| D2 | 交付形态 | Library + Kotlin DSL（C++ 是 source of truth） | 同时服务 Android 和嵌入式 Linux |
| D3 | Workflow 模型 | DAG + activation_group/condition（autogen #6711 风格） | 拓扑可静态分析；循环用 SCC 分组解决回边阻塞问题 |
| D4 | State 形态 | Protobuf message | 跨 JNI 零拷贝、版本演进、与 LiteRT-LM 自身一致 |
| D5 | LLM session 状态 | 节点私有 LiteRtLmSession（不进 graph state） | KV cache 是推理细节，不应泄漏到 workflow 层 |
| D6 | Multi-agent | Agent=Node 默认 + TeamNode（LLM-select / state-router / parallel-gather） | 90% 场景 agent 直接是 node；不要 round-robin（与 graph 循环边重复） |
| D7 | Tool 系统 | MCP + 原生 tool 并存，schema 在注册点显式声明 | MCP 接生态 tool；native tool 接宿主 app 业务函数 |
| D8 | Streaming | 一等公民（token-level event） | 端侧 chat UX 必需 |
| D9 | Cancellation | 节点必须 cancel-aware；LLM 节点框架自动接 LiteRT-LM abort | 协作式取消；强制取消不现实 |
| D10 | C++ 标准 | C++20 + exception + RTTI | coroutine 对 streaming/cancel 是降维打击 |
| D11 | Persistence | 内置 checkpoint hook，不自动 resume | 显式 resume 避免陈旧 state 被默默使用 |
| D12 | Observability | 内置 TraceEvent API（接 Perfetto/OTel） | UI 可视化交给生态 |
| D13 | 构建系统 | CMake + git submodule | 与 LiteRT-LM 构建栈一致 |

---

## 3. 整体架构

### 3.1 模块分层

```
┌─────────────────────────────────────────────────────────────┐
│  Kotlin DSL Layer (Android only)                            │
│  • workflow{} / agent{} / tool{} DSL                        │
│  • Flow<TraceEvent>（含 TokenEvent / NodeEvent / 等）       │
│  • 通过 JNI 调 C++ public API；零业务逻辑                    │
└────────────────────────┬────────────────────────────────────┘
                         │  JNI (薄)
                         ▼
┌─────────────────────────────────────────────────────────────┐
│  C++ Public API (libagentflow.so) — Source of Truth         │
│                                                             │
│  agentflow/                                                 │
│  ├── core/        Graph, Node, Edge, ActivationGroup,       │
│  │                State, Runner, Token/Stream/Cancellation  │
│  ├── nodes/       LlmNode, ToolNode, AgentNode, TeamNode,   │
│  │                RouterNode, AggregatorNode                │
│  ├── inference/   LiteRtLmEngine wrapper (Session 管理,      │
│  │                streaming decode, abort hook)             │
│  ├── tools/       ToolRegistry, McpToolAdapter,             │
│  │                NativeFnTool, schema validation           │
│  ├── trace/       TraceEvent, TraceEmitter (Perfetto/OTel)  │
│  └── persist/     CheckpointWriter (sqlite), ResumeLoader   │
└────────┬──────────────────────────────────┬─────────────────┘
         │                                  │
         ▼                                  ▼
┌────────────────────┐            ┌──────────────────────┐
│  LiteRT-LM (subm)  │            │  gopher-mcp (subm)   │
│  C ABI: engine.h   │            │  C++ class + C ABI   │
│  prefill/decode/   │            │  stdio/SSE/WS/TCP    │
│  conversation/tool │            │                      │
└────────────────────┘            └──────────────────────┘
```

### 3.2 关键边界规则

1. **`core/` 不知道 LiteRT-LM 和 MCP**：core 只依赖 `Node` 接口；`inference/` 和 `tools/` 是 core 的实现者。换推理引擎或 MCP 实现不破坏 core API。
2. **JNI 层零业务逻辑**：JNI 只做 protobuf 字节数组 marshal + 调一次 C++ public API。
3. **跨边界数据全部 protobuf**：State、tool schema、trace event、graph spec、错误事件都是 protobuf，C++/Kotlin 共用 .proto。
4. **Submodule 不修改**：LiteRT-LM 和 gopher-mcp 锁定 commit；所有适配在 `inference/` 和 `tools/` 里做。

---

## 4. 核心 C++ 抽象

本节定义的类型是 C++ 公开头文件，也是 Kotlin DSL 通过 JNI 间接操作的对象。

### 4.1 State

```cpp
// agentflow/core/state.h
namespace agentflow {

class State {
 public:
  template <typename ProtoT>
  static State From(ProtoT msg);

  template <typename ProtoT>
  const ProtoT& As() const;

  template <typename ProtoT>
  ProtoT& Mutable();

  std::string SerializeAsString() const;
  bool ParseFromString(std::string_view data);

 private:
  std::unique_ptr<google::protobuf::Message> msg_;
};

}  // namespace agentflow
```

**并发约束**：State 在节点执行期间是该节点的独占视图。Runner 负责 fan-out 时拷贝、fan-in 时合并（默认用户提供 `merge(State&, const State&)`，缺省后到覆盖）。节点内部不需要加锁。

### 4.2 Node

```cpp
// agentflow/core/node.h
class Node {
 public:
  using NodeId = std::string;

  virtual asio::awaitable<State> Run(
      State state,
      const CancelToken& cancel,
      EventEmitter& emit) = 0;

  virtual std::string_view Kind() const = 0;
  virtual NodeId Id() const = 0;

  virtual ~Node() = default;
};
```

**为什么是 coroutine（`asio::awaitable<T>`）**：
- `co_await cancel.WaitCancelled()` 让 cancel 传播自然
- LLM streaming decode 写成 `while (auto t = co_await session.NextTokenAsync())` 不阻塞 worker 线程
- fan-out / fan-in 用 `asio::experimental::make_parallel_group` 表达干净

**asio 选择理由**：Android NDK r25+ 已支持 C++20 coroutine；asio 是协程基础设施最成熟的 C++ 库；不依赖 Boost。

### 4.3 Edge + ActivationGroup

```cpp
// agentflow/core/edge.h
struct Edge {
  Node::NodeId from;
  Node::NodeId to;

  // 0  = 不在循环里（普通 DAG 边）
  // >0 = 属于第 N 个 SCC（由 Graph::Compile 自动分配；用户可显式覆盖）
  int activation_group = 0;

  enum class Condition { ALL, ANY };
  Condition condition = Condition::ALL;
};
```

**激活语义**：节点 ready 当且仅当——对每个 activation_group，该 group 内所有边的剩余计数都满足该 group 的 condition。Runner 维护 `map<NodeId, map<int, int>>` 记录剩余计数。

**SCC 分组算法**：`Graph::Compile()` 跑 Tarjan SCC，对所有非平凡 SCC（节点数 ≥ 2 或有自环）内的边自动分配 group_id（从 1 递增）。SCC 间的边、以及非 SCC 内的边 group=0。用户可通过 `AddEdge(from, to, user_group, cond)` 覆盖默认分组。

### 4.4 Graph 与 GraphBuilder

```cpp
// agentflow/core/graph.h
class Graph {
 public:
  static GraphBuilder Builder();

  absl::Status Compile();  // SCC 分组 + 验证

  std::span<const Node*> Nodes() const;
  std::span<const Edge> Edges() const;

  std::string ToDotString() const;  // graphviz dump，含 group 标注（调试用）
};

class GraphBuilder {
 public:
  GraphBuilder& AddNode(std::unique_ptr<Node> node);
  GraphBuilder& AddEdge(NodeId from, NodeId to,
                        Edge::Condition cond = Edge::Condition::ALL);
  GraphBuilder& AddEdge(NodeId from, NodeId to,
                        int user_group, Edge::Condition cond);

  Graph Build();  // 等价于 AddNode/AddEdge 后自动调 Compile
};
```

**Compile 阶段验证**：
- 每个节点的所有入边在同一 group 内 condition 必须一致（否则报 `GraphCompileError`）
- 节点 id 唯一
- 至少一个 entry 节点（无入边或仅 group=0 入边都消耗完）
- （可选）边的 input/output 类型兼容性

### 4.5 Runner

```cpp
// agentflow/core/runner.h
class Runner {
 public:
  enum class CheckpointPolicy {
    Off,                  // 不写 checkpoint
    AfterEachNode,        // 每个节点完成后写一次（state + 已完成节点集合）
    AfterTerminalNode,    // 仅在 sink 节点完成时写
  };

  struct Options {
    int max_concurrent_nodes = 4;
    std::chrono::milliseconds node_timeout{0};       // 0 = no timeout
    CheckpointPolicy checkpoint = CheckpointPolicy::Off;
    std::string checkpoint_path;                     // 仅 checkpoint != Off 时使用
    TraceEmitter* trace = nullptr;
    asio::any_io_executor executor;                   // 用户可注入自己的 executor
  };

  Runner(Graph graph, Options opts);

  asio::awaitable<State> Run(State initial, CancelToken cancel = {});

  EventStream RunStreaming(State initial, CancelToken cancel = {});

  asio::any_io_executor Executor() const;
};
```

`RunStreaming` 返回的 `EventStream` 是一个 `asio` channel，runner 推送 `NodeStartEvent / NodeEndEvent / TokenEvent / ToolCallEvent / ToolReturnEvent / EdgeFireEvent / FinalEvent`。调用方读完 channel 即得到完整执行轨迹。

### 4.6 CancelToken

```cpp
// agentflow/core/cancel.h
class CancelToken {
 public:
  bool IsCancelled() const noexcept;
  asio::awaitable<void> WaitCancelled() const;
  void OnCancel(std::function<void()> cb);  // LLM 节点用这个把 abort 接进来
};

class CancelSource {
 public:
  CancelToken Token() const;
  void Cancel();  // 触发所有 OnCancel 回调，使 IsCancelled() 返回 true
};
```

### 4.7 Event / Trace

```cpp
struct TraceEvent {
  enum Kind { NodeStart, NodeEnd, Token, ToolCall, ToolReturn, EdgeFire,
              NodeFailed, GraphDone };
  Kind kind;
  std::string node_id;
  std::chrono::steady_clock::time_point ts;
  std::string payload;  // 序列化的 protobuf；具体类型由 kind 决定
};

class EventEmitter {
 public:
  void Emit(TraceEvent ev);
  void EmitToken(NodeId, std::string_view token);
  void EmitToolCall(NodeId, std::string_view tool, std::string_view args_json);
  void EmitToolReturn(NodeId, std::string_view tool, std::string_view result_json);
};
```

---

## 5. 节点实现

### 5.1 LlmNode — LiteRT-LM 推理桥

```cpp
// agentflow/nodes/llm_node.h
class LlmNode : public Node {
 public:
  struct Config {
    std::shared_ptr<LiteRtLmEngine> engine;     // 共享引擎
    std::string system_prompt;
    std::string state_field_in;                 // protobuf field 名（state 输入）
    std::string state_field_out;                // protobuf field 名（state 输出）
    std::string state_messages_field;           // 对话历史字段（可选）
    std::vector<std::string> available_tools;   // ToolRegistry 中的 tool 名
    LiteRtLmSamplerParams sampler;
    int max_output_tokens = 512;
    bool stream_tokens = true;
  };

  asio::awaitable<State> Run(State, const CancelToken&, EventEmitter&) override;
};
```

**核心实现**（伪代码）：

```cpp
asio::awaitable<State> LlmNode::Run(State s, const CancelToken& cancel,
                                     EventEmitter& emit) {
  auto session = engine_->CreateSession(cfg_.sampler, cfg_.max_output_tokens);
  cancel.OnCancel([&session] { session.Abort(); });

  auto conv = BuildConversationConfig(s, cfg_);  // system + tools_json + history
  co_await session.PrefillAsync(conv);

  std::string accum;
  while (!session.Done()) {
    if (cancel.IsCancelled()) co_return State{};
    auto tok = co_await session.NextTokenAsync();
    accum += tok;
    if (cfg_.stream_tokens) emit.EmitToken(Id(), tok);
  }

  auto parsed = ParseLlmOutput(accum);
  if (parsed.tool_call.has_value()) {
    UpdateStateWithToolCall(s, *parsed.tool_call);
  } else {
    UpdateStateWithReply(s, accum);
  }
  co_return std::move(s);
}
```

**关键设计**：LlmNode 不调用 tool。它只输出"模型说要调 tool X 参数 Y"。Tool 调用是 AgentNode 内部循环（或下游 ToolNode）的事。这让 LlmNode 在非 agent 场景（纯 completion）也能直接用。

### 5.2 AgentNode — ReAct 循环封装

```cpp
class AgentNode : public Node {
 public:
  struct Config {
    LlmNode::Config llm;
    std::shared_ptr<ToolRegistry> tools;
    int max_iter = 8;
    bool emit_inner_events = true;  // 把内部 LLM/tool 事件冒泡到外层 emit
  };

  asio::awaitable<State> Run(State s, const CancelToken& cancel,
                              EventEmitter& emit) override {
    LlmNode llm{cfg_.llm};
    for (int i = 0; i < cfg_.max_iter; ++i) {
      if (cancel.IsCancelled()) co_return std::move(s);
      s = co_await llm.Run(std::move(s), cancel, emit);
      auto tc = ExtractPendingToolCall(s);
      if (!tc) co_return std::move(s);  // LLM 输出最终答案
      auto result = co_await cfg_.tools->Invoke(tc->name, tc->args, cancel);
      AppendToolResultToHistory(s, *tc, result);
    }
    co_return std::move(s);  // max_iter 达到
  }
};
```

**对外是单 graph node**（tool 循环不污染外层拓扑）；内部跑 reason → act → observe 循环。

### 5.3 TeamNode — 动态多 agent 调度

```cpp
class TeamNode : public Node {
 public:
  enum class Policy { LlmSelect, StateRouter, ParallelGather };

  struct Config {
    Policy policy;
    std::vector<std::unique_ptr<Node>> members;
    int max_turns = 10;                              // LlmSelect 用
    LlmNode::Config moderator_llm;                   // LlmSelect 用
    std::function<Node::NodeId(const State&)> router;             // StateRouter 用
    std::function<State(std::vector<State>)> aggregator;          // ParallelGather 用
  };

  asio::awaitable<State> Run(State, const CancelToken&, EventEmitter&) override;
};
```

**TeamNode 不递归调用 Runner**。内部直接 `co_await member->Run(...)`。理由：成员是已构造好的 Node，没有 graph 边/SCC 概念，不需要 runner 调度开销。复杂嵌套场景由用户显式包装一个"嵌套 Graph"节点解决。

### 5.4 RouterNode / AggregatorNode

辅助节点。`RouterNode` 读 state 后给定下游 NodeId（用于 conditional edge）。`AggregatorNode` 收 fan-in 多个 state 调用用户函数合并。两者都是 thin wrapper，逻辑都是用户函数。

---

## 6. Tool 系统

### 6.1 Tool 接口

```cpp
// agentflow/tools/tool.h
struct ToolSchema {
  std::string name;
  std::string description;
  std::string params_json_schema;
  std::string returns_json_schema;
};

class Tool {
 public:
  virtual const ToolSchema& Schema() const = 0;
  virtual asio::awaitable<std::string> Invoke(
      std::string_view args_json,
      const CancelToken& cancel) = 0;
  virtual ~Tool() = default;
};
```

### 6.2 NativeFnTool — In-process 实现

```cpp
class NativeFnTool : public Tool {
 public:
  using Fn = std::function<asio::awaitable<std::string>(
      std::string_view, const CancelToken&)>;

  NativeFnTool(ToolSchema schema, Fn fn);
};
```

Kotlin lambda 通过 callback 桥包装成 `Fn`：

```cpp
// jni/agentflow_jni.cc 节选
NativeFnTool MakeKotlinTool(JNIEnv*, jobject lambda, ToolSchema schema) {
  auto global = env->NewGlobalRef(lambda);
  return NativeFnTool(std::move(schema),
    [global](std::string_view args, const CancelToken&) -> asio::awaitable<std::string> {
      auto* env = AttachCurrentThreadAsDaemon();
      auto jresult = env->CallObjectMethod(global, ..., args);
      co_return JStringToStd(env, jresult);
    });
}
```

### 6.3 McpToolAdapter — 接 gopher-mcp

```cpp
class McpToolAdapter : public Tool {
 public:
  McpToolAdapter(std::shared_ptr<gopher::McpClient> client,
                 std::string remote_tool_name,
                 ToolSchema cached_schema);
  // ...
};
```

一个 `McpToolAdapter` 对应远端的一个 tool。多个 adapter 共享同一个 `McpClient`（同 server 暴露多个 tool 时复用连接）。

### 6.4 ToolRegistry

```cpp
class ToolRegistry {
 public:
  void Register(std::shared_ptr<Tool> tool);

  asio::awaitable<absl::Status> AttachMcpServer(McpServerSpec spec);

  asio::awaitable<std::string> Invoke(std::string_view name,
                                       std::string_view args_json,
                                       const CancelToken&);

  std::string ExportToolsJsonForLlm(std::span<const std::string> visible) const;
};

struct McpServerSpec {
  enum class Transport { Stdio, HttpSse, WebSocket, Tcp };
  Transport transport;
  // stdio: command + args；http_sse/ws/tcp: url 或 host+port
  std::string command_or_url;
  std::vector<std::string> args;
};
```

**端侧优化**：
- McpClient 在 ToolRegistry 内池化（同 server 一个 client）
- stdio MCP server 默认 lazy-start（首次调用才 fork）
- `AttachMcpServer` 失败不阻塞 graph 启动（log warn + 该 tool 在 LLM 看到的 schema 中剔除）

---

## 7. Kotlin DSL

### 7.1 用户 DSL 示例

```kotlin
val graph = workflow {
    state(MyState::class)

    val planner = agent("planner") {
        engine = sharedEngine
        systemPrompt = "你是任务规划师..."
        readField = MyState::userQuery
        writeField = MyState::plan
        sampler { topK = 40; temperature = 0.7f }
    }

    val researcher = agent("researcher") {
        tools(McpServer.stdio("rg-mcp", "/usr/bin/rg-mcp"))
        tools(nativeTool("local_db") { args -> queryLocalDb(args) })
        maxIter = 6
    }

    val reviewer = llmNode("reviewer") { /* ... */ }
    val merger = router("merger") { state ->
        if (state.confidence > 0.8) "done" else "researcher"
    }

    edges {
        planner then researcher
        researcher then reviewer
        reviewer then merger
        merger.case("done") then sink
        merger.case("researcher") then researcher  // 回边 → SCC
    }
}.compile()

graph.run(initialState).collect { event ->
    when (event) {
        is TokenEvent -> ui.appendToken(event.nodeId, event.token)
        is NodeEnd    -> log.info("node ${event.nodeId} done")
        is ToolCall   -> log.info("calling ${event.name}")
        is Final      -> done(event.state)
    }
}
```

### 7.2 翻译规则

- DSL **声明式收集**，不持有图状态；`compile()` 时一次性 marshal 成 protobuf `GraphSpec` 调到 C++ `GraphBuilder`
- `Flow<TraceEvent>` 由 `callbackFlow` 包装 C++ event channel
- 协程 cancel → `awaitClose { NativeBridge.runnerCancel(...) }` → C++ `CancelSource.Cancel()`

### 7.3 DSL 边界（明确写死避免 scope 蔓延）

- ❌ 不在 Kotlin 里实现节点行为（节点必须是 C++ 类，Kotlin 只声明）
  - 例外：用户注册的 native tool lambda（callback 桥）
- ❌ 不在 Kotlin 里读写 state 的中间值（state 在 C++ 端流转，Kotlin 只看 event 和最终 state）
- ❌ 不重复实现 graph 调度（runner 只在 C++）

---

## 8. JNI 边界

### 8.1 设计原则

JNI 函数只做"调一次 C++ public API"。不在 JNI 内做循环、字符串处理或业务逻辑。所有跨界数据走 protobuf 字节数组。

### 8.2 JNI 函数列表（节选）

```kotlin
internal object NativeBridge {
    init { System.loadLibrary("agentflow_jni") }

    external fun graphBuilderCreate(): Long
    external fun graphBuilderAddNode(handle: Long, nodeSpecProto: ByteArray): String
    external fun graphBuilderAddEdge(handle: Long, edgeProto: ByteArray)
    external fun graphBuilderCompile(handle: Long): Long
    external fun graphBuilderDestroy(handle: Long)

    external fun runnerCreate(graphHandle: Long, optsProto: ByteArray): Long
    external fun runnerStart(runnerHandle: Long, initialStateProto: ByteArray, callback: Long)
    external fun runnerCancel(runnerHandle: Long)
    external fun runnerDestroy(runnerHandle: Long)

    external fun toolRegistryCreate(): Long
    external fun toolRegistryRegisterNative(handle: Long, schemaProto: ByteArray, lambda: Any): Long
    external fun toolRegistryAttachMcp(handle: Long, mcpSpecProto: ByteArray): Long
}
```

句柄全部 `Long`（C++ 端 `unique_ptr` / `shared_ptr` 持有，对应 destroy 函数释放）。

### 8.3 异步 Runner → Kotlin Flow 桥接

```cpp
// 关键 JNI 函数
extern "C" JNIEXPORT void JNICALL
Java_..._runnerStart(JNIEnv* env, jobject, jlong runner_h,
                      jbyteArray init, jlong cb_global) {
  auto* runner = reinterpret_cast<Runner*>(runner_h);
  State state = ParseState(env, init);

  asio::co_spawn(runner->Executor(),
    [runner, state = std::move(state), cb_global]() -> asio::awaitable<void> {
      auto stream = runner->RunStreaming(std::move(state));
      while (auto ev = co_await stream.NextAsync()) {
        InvokeJavaCallback(cb_global, *ev);   // 推到 Kotlin callbackFlow
      }
      InvokeJavaCallback(cb_global, FinalEvent{stream.FinalState()});
    },
    asio::detached);
}
```

Kotlin 端：

```kotlin
fun Graph.run(initial: State): Flow<TraceEvent> = callbackFlow {
    val cb = registerCallback { event -> trySend(event) }
    val runnerHandle = NativeBridge.runnerCreate(this@run.handle, defaultOpts())
    NativeBridge.runnerStart(runnerHandle, initial.toByteArray(), cb)
    awaitClose {
        NativeBridge.runnerCancel(runnerHandle)
        NativeBridge.runnerDestroy(runnerHandle)
        unregisterCallback(cb)
    }
}
```

### 8.4 线程模型

- C++ 端：runner 持有 asio thread pool（默认 `max_concurrent_nodes` 个线程）
- JNI 回调进 Java：`AttachCurrentThreadAsDaemon` + 缓存的 `JavaVM*`；callback 是 `GlobalRef`
- Kotlin 端 `Flow.collect` 通常在 `Dispatchers.Main` 或用户指定 dispatcher
- **不在 JNI callback 里跑业务逻辑**：trySend 之后立刻返回 C++

---

## 9. 错误处理

### 9.1 C++ 错误层级

```cpp
// agentflow/core/errors.h
class AgentflowError : public std::runtime_error { /* ... */ };
class GraphCompileError : public AgentflowError {};   // SCC 验证、schema 不一致
class LlmError : public AgentflowError {};
class LlmAbortedError : public LlmError {};           // cancel 触发
class LlmOomError : public LlmError {};
class ToolError : public AgentflowError {};
class McpTransportError : public ToolError {};
```

### 9.2 节点失败传播规则

1. Node `Run()` 抛异常 → runner 捕获 → emit `NodeFailedEvent` → 标记节点失败
2. **默认行为**：失败传播 = 整个 graph 终止（清理所有运行中节点 → emit `GraphDone` 带 error）
3. 节点可声明 `failure_policy: Retry(n) | Fallback(node_id) | Continue`（NodeConfig 字段）
4. 取消触发的失败（`LlmAbortedError`）不算"失败"，emit `NodeEnd` with cancelled=true

### 9.3 跨 JNI 错误传递

JNI 边界必须 noexcept：

```cpp
#define JNI_CATCH(env, ret) \
  catch (const std::exception& e) { ThrowJavaException(env, e.what()); return ret; } \
  catch (...) { ThrowJavaException(env, "unknown C++ error"); return ret; }
```

Error event 也走 protobuf：

```protobuf
message ErrorEvent {
  string node_id = 1;
  string type = 2;     // "LlmAbortedError" 等
  string message = 3;
  string trace = 4;    // 可选 stack trace
}
```

Kotlin 侧：

```kotlin
sealed class AgentflowException(msg: String) : Exception(msg)
class LlmAbortedException(msg: String) : AgentflowException(msg)
class McpTransportException(msg: String) : AgentflowException(msg)
class GraphCompileException(msg: String) : AgentflowException(msg)
```

---

## 10. Protobuf Schema

`proto/` 目录是跨语言 source of truth。CMake 同时跑 `protoc` 生成 C++ 和 Java/Kotlin（Android 端用 `protobuf-lite`）。

```
proto/
├── state.proto          # 用户在自己的 .proto 中 import 这个
├── trace_event.proto    # TraceEvent / TokenEvent / NodeEvent / FinalEvent
├── tool.proto           # ToolSchema, ToolCall, ToolResult
├── graph_spec.proto     # GraphBuilder 跨 JNI 用的中间表示
├── mcp_spec.proto       # McpServerSpec
└── errors.proto         # ErrorEvent
```

Trace event 单独保留 full protobuf（需要 reflection），其他 hot-path 用 lite。

---

## 11. 项目结构

```
zen/
├── CMakeLists.txt                            # 顶层
├── third_party/
│   ├── LiteRT-LM/                            # 已存在 submodule
│   └── gopher-mcp/                           # 新增 submodule
├── proto/                                    # 跨语言 schema
├── agentflow/                                # C++ 库
│   ├── CMakeLists.txt
│   ├── core/
│   │   ├── state.{h,cc}
│   │   ├── node.{h,cc}
│   │   ├── edge.{h,cc}
│   │   ├── graph.{h,cc}                      # 含 SCC 分组
│   │   ├── runner.{h,cc}
│   │   ├── cancel.{h,cc}
│   │   ├── event.{h,cc}
│   │   └── errors.{h,cc}
│   ├── nodes/
│   │   ├── llm_node.{h,cc}
│   │   ├── agent_node.{h,cc}
│   │   ├── team_node.{h,cc}
│   │   ├── router_node.{h,cc}
│   │   └── aggregator_node.{h,cc}
│   ├── inference/
│   │   ├── litert_lm_engine.{h,cc}
│   │   ├── litert_lm_session.{h,cc}          # 异步 Prefill/NextToken awaitable 封装
│   │   └── tool_call_parser.{h,cc}
│   ├── tools/
│   │   ├── tool.{h,cc}
│   │   ├── native_fn_tool.{h,cc}
│   │   ├── mcp_tool_adapter.{h,cc}
│   │   ├── mcp_client_pool.{h,cc}
│   │   └── tool_registry.{h,cc}
│   ├── trace/
│   │   ├── trace_emitter.{h,cc}
│   │   └── perfetto_emitter.{h,cc}           # 可选编译
│   └── persist/
│       ├── checkpoint.{h,cc}
│       └── sqlite_store.{h,cc}
├── jni/
│   ├── CMakeLists.txt
│   ├── agentflow_jni.cc                      # 所有 JNI 函数集中
│   └── jni_helpers.{h,cc}
├── kotlin/
│   ├── build.gradle.kts
│   ├── src/main/kotlin/agentflow/
│   │   ├── dsl/
│   │   ├── runtime/
│   │   ├── jni/NativeBridge.kt
│   │   └── proto/                            # gradle plugin 生成
│   └── src/test/kotlin/...
├── examples/
│   ├── chat-android/
│   ├── researcher-cli/                       # 纯 C++ 例子
│   └── multi-agent-team/
├── tests/
│   ├── unit/
│   ├── integration/
│   └── e2e/
└── docs/
    ├── design/                               # 本文档及后续 ADR
    └── api/                                  # Doxygen + dokka 输出
```

---

## 12. 测试策略

| 层 | 范围 | 框架 | 速度 | 数量 |
|---|---|---|---|---|
| Unit (C++) | State / Edge / SCC 分组 / Runner 调度 | GoogleTest | 毫秒级 | 多 |
| Unit (JNI) | JNI marshalling 不丢数据 | GoogleTest + JNI in-test JVM | 秒级 | 中 |
| Unit (Kotlin) | DSL builder 翻译成正确 GraphSpec proto | JUnit + kotlinx-coroutines-test | 秒级 | 中 |
| Integration | Runner + 假 LlmNode/ToolNode（stub 模型） | GoogleTest | 秒级 | 中 |
| E2E | 真 LiteRT-LM 加载小模型 + stdio MCP server | pytest + adb shell | 分钟级 | 少 |

### 12.1 关键测试点

1. **SCC 分组正确性**：随机生成图，对照 NetworkX 的 SCC 结果做 property-based 验证
2. **激活计数收敛**：测试矩阵覆盖纯 DAG / 单 SCC + 外部入口 / 嵌套 SCC
3. **Cancel 传播**：LlmNode 启动 → cancel → 验证 LiteRtLmSession.Abort 被调用 + Run() ≤100ms 内 co_return
4. **JNI 不漏对象引用**：leak sanitizer + JNI weak ref 检测所有 handle / GlobalRef 释放
5. **MCP server 启动失败不阻塞**：stub 一个会 spawn 失败的 stdio command，graph 仍能跑（不可用 tool 在 LLM schema 中剔除）

### 12.2 不写测试

- LiteRT-LM 自身正确性（它有自己的测试）
- gopher-mcp 协议合规性（同上）
- Protobuf 序列化往返

---

## 13. 已知风险与待验证假设

| # | 风险 | 影响 | 缓解 |
|---|---|---|---|
| R1 | LiteRT-LM C API 不一定有真正异步 decode + abort | 高 — streaming/cancel 实现可能要 polling 包装 | 实现初期 spike：通读 `c/engine.h`，确认 decode 是 callback / blocking / 可中断；blocking 则用专用线程 + abort flag 包装 |
| R2 | gopher-mcp C++ API 稳定性（项目较新） | 中 | 锁 commit；适配层是唯一接触面，change blast radius 受限 |
| R3 | Android NDK 上 asio coroutine 二进制大小 | 中 — 库可能膨胀几 MB | 实测后决定；如太大回退到 std::thread + 手写 future channel；coroutine 是优化项不是必需 |
| R4 | protobuf-lite 不支持 reflection | 低 | trace event 单独用 full protobuf；其他 hot-path 用 lite |
| R5 | Kotlin Flow 取消传播到 C++ 的延迟 | 低 | callback 收到 cancel 立刻 trySend cancel sentinel；C++ onCancel 立刻 LiteRT-LM Abort |
| R6 | SCC 分组对用户不可见，调试困难 | 中 | `Graph.toDotString()` 输出 graphviz；`Compile` 后能 dump 分组结果 |
| R7 | 长对话 KV cache 跨 Run 不复用 | 中 — 每次都 prefill 全 history | v1 不解决；v2 加 `session_persistence` 选项让 LlmNode 持有长 session |

---

## 14. 不在 v1 范围（YAGNI）

- 嵌套子 graph 一等支持（Team 已覆盖 90%）
- 自动 resume（决策已定）
- Trace UI 可视化工具（Perfetto 够用）
- 跨进程 graph 分布式执行
- Embedding / RAG / vectorstore（独立项目）
- Round-robin 多 agent 调度（用 graph 循环边表达）
- Web UI / REST 网关
- iOS / Swift binding（Kotlin Multiplatform 是 v2 话题）
- 长 session（KV cache 跨 Run 复用）— 见 R7
- 模型热切换 / 多模型并存调度

---

## 15. v1 验收标准

完成定义为以下 demo 都能在 Android 真机和嵌入式 Linux 上跑通：

1. **Demo A — 单 agent + MCP**（Android, Kotlin DSL）：一个 ReActAgent + filesystem MCP server (stdio)，回答"列出 /sdcard 里所有 jpg 并按大小排序"，token streaming 显示。
2. **Demo B — 多 agent Team**（Android, Kotlin DSL）：planner → researcher (with tools) → writer，state 通过整条链；TeamNode 用 LlmSelect 在 researcher 和 critic 之间循环不超过 3 轮。
3. **Demo C — 纯 C++ CLI**（嵌入式 Linux ARM64）：同样 graph 用纯 C++ API 组装运行，证明 Kotlin 不是必需。
4. **取消正确性**：Demo A 在解码途中按返回键，1 秒内 Session.Abort + 所有协程退出，无 leak。
5. **崩溃恢复**：Demo B 在 researcher 节点完成后 `kill -9`，重启用 checkpoint manual resume 能从 reviewer 节点继续。

---

## 附录 A — 参考资料

- LiteRT-LM C API: `third_party/LiteRT-LM/c/engine.h`
- gopher-mcp: https://github.com/GopherSecurity/gopher-mcp
- AutoGen activation_group PR: https://github.com/microsoft/autogen/pull/6711
- LangGraph state graph 模型（仅作概念对照，未采用）
- asio C++20 coroutine: https://think-async.com/Asio/asio-1.30.2/doc/asio/overview/composition/cpp20_coroutines.html
