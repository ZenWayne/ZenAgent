# P6 — LiteRT-LM Conversation API：正确的对话模板 + 工具调用解析

**分支：** `feat/p6-conversation`
**目标：** 将我们现在基于原始 Session 的 prompt 流程替换为 LiteRT-LM 的结构化 Conversation API，让模型收到正确套用聊天模板的 prompt（system + tools + history），引擎返回的 JSON 也可以被框架直接解析，不再需要猜测 Gemma 的 `<|tool_call>...<tool_call|>` 标记。

P5 把可观测性通道补齐了。P6 要补的是「我们在用裸 JSON 朝一个对话模型大喊」这道缺口——这正是 agent-demo 现在吐出 `],"name":"assistant"}]}}` 而不是真实回答的根因。推理链路本身是 *通的*（Prefill OK、Decode 在流式吐 token、AgentNode 也收到了），但模型看到的是一段错乱的 prompt，因为我们从来没套用模型自己的对话模板。

## 今天的问题

`AgentNode::BuildConversationJson` 直接拼成一个 JSON 对象：
```json
{"messages":[...],"max_tokens":1024,"stream":true,"tools":[...]}
```
…然后原样塞给 `LiteRtLmSession::Start(text)`。Session 把这个字符串当裸文本输入处理。Gemma 的 tokenizer 从来没见过 `<|turn>system`、`<|tool>...<tool|>`、`<|turn>user`、`<|turn>model` 这些标记——它就只是顺着这串 JSON 字面量继续往下写。所以回复里会出现那段无意义的尾巴。

就算真的有正常回复，`AgentNode::Run` 会拿 `accum` 去匹配 `{"tool_calls":[{"id":...,"function":{"name":...,"arguments":...}}]}`（OpenAI 的形状），但 Gemma 按它自己的模板吐的是 `<|tool_call>call:name{k:v}<tool_call|>`。匹配永远 miss → AgentNode 永远不会派发工具 → 「最终答案」兜底分支直接把那段乱码打印出来。

## 我们手里已有的能力

`third_party/litert_lm/include/c/engine.h` 在 Session 之上暴露了第二层 API：

```c
LiteRtLmConversationConfig* litert_lm_conversation_config_create(
    engine, session_config,
    const char* system_message_json,
    const char* tools_json,
    const char* messages_json,
    bool enable_constrained_decoding);

LiteRtLmConversation* litert_lm_conversation_create(engine, config);
int litert_lm_conversation_send_message_stream(
    conversation, const char* message_json, const char* extra_context,
    LiteRtLmStreamCallback cb, void* cb_data);
void litert_lm_conversation_cancel_process(conversation);
```

引擎 dump 出来的 config 里能看到 `use_template_for_fc_format: true`，模型自己的 `.litertlm` 文件里也烤了一份 `jinja_prompt_template`。改用 Conversation 就能让引擎自动套这个模板，并把工具调用解析回结构化 JSON（响应类型是 `LiteRtLmJsonResponse`，通过 `litert_lm_json_response_get_string` 读字符串）。

## 文件结构

```
agentflow/
  inference/
    litert_lm_conversation.{h,cc}        # 新增 —— Conversation API 的 C++ 封装
    litert_lm_session.{h,cc}             # 保留 —— 希望用裸 Session 的 LlmNode 使用者还能用
  nodes/
    agent_node.{h,cc}                    # 修改 —— Run 循环改走 Conversation,去掉 BuildConversationJson
    llm_node.{h,cc}                      # 修改 —— 同理,单次调用版本
examples/
  agent-demo/main.cc                     # 修改 —— 验证 get_time 工具能拿到真实时间
tests/unit/
  nodes/
    agent_node_test.cc                   # 修改 —— DISABLED → 改成 MODEL_PATH 门控的真模型测试
    llm_node_test.cc                     # 修改 —— 同上
docs/superpowers/plans/
  2026-06-03-cpp-agent-framework-p6-conversation-api.md    # 原计划文件（英文）
  2026-06-03-cpp-agent-framework-p6-conversation-api.zh.md # 本文件
```

## 任务依赖

```
T1 (Conversation 封装) ──→ T2 (探响应 schema)
                           │
                           ├──→ T3 (LlmNode 重构) ──→ T5 (测试)
                           └──→ T4 (AgentNode 重构) ─┘
                                                     ▼
                                                   T6 (demo) ──→ T7 (打 tag + PR)
```

## Task 1：`LiteRtLmConversation` 封装

**文件：** `agentflow/inference/litert_lm_conversation.{h,cc}`

### Step 1.1：头文件

```cpp
namespace agentflow {

struct LiteRtLmConversationOptions {
  std::string system_message_json;   // 例如 {"role":"system","content":"..."}
  std::string tools_json;            // 例如 [{"type":"function","function":{...}}, ...]（无工具就传 []）
  std::string messages_json;         // 初始历史（从头开始就传 []）
  bool enable_constrained_decoding = false;
  int max_output_tokens = 1024;
};

class LiteRtLmConversation {
 public:
  static std::shared_ptr<LiteRtLmConversation> Create(
      std::shared_ptr<LiteRtLmEngine> engine,
      LiteRtLmConversationOptions opts,
      asio::io_context& io_ctx);

  ~LiteRtLmConversation();

  // 流式吐回复 chunk。每个 token 调用都会让一次 NextTokenAsync resolve,
  // 行为与 LiteRtLmSession 完全对齐。流结束后 NextTokenAsync 返回空串。
  // 流结束后可通过 `FullResponseJson()` 拿到模型完整的 JSON 响应
  //（包含已解析的 tool_calls,如果有的话）。
  void SendMessage(std::string message_json, std::string extra_context = "");
  asio::awaitable<std::string> NextTokenAsync();
  std::string FullResponseJson() const;  // 流结束后填好
  void Cancel();

 private:
  /* opaque LiteRtLmConversation*, asio channel, io_ctx 引用, accum buffer */
};
}
```

实现照搬 `LiteRtLmSession`——`asio::experimental::channel<void(asio::error_code, std::string)>`，回调里 `asio::post` 把 token 抛回 io_ctx（LiteRT 会从它自己的 worker 线程回调，channel 不是线程安全的）。

### Step 1.2：BUILD 接好 + 提交

挂进 `agentflow/inference/BUILD.bazel` 已有 target。先单独把它 build 一遍，确认头文件能链上。

提交信息：`feat: LiteRtLmConversation wrapper around C engine conversation API`

## Task 2：探一下响应实际长什么样

重构节点之前，先写个 30 行左右的临时 binary，拿 get_time 工具的 prompt 喂给 `LiteRtLmConversation`，把这两样东西打出来：
- 流过来的 chunk（裸字节）。
- 流结束后 `FullResponseJson()` 拿到的完整串。

我们目前并不知道 LiteRT-LM 返回的 JSON 究竟长什么样。可能的候选：`{"text":"...","tool_calls":[...]}`、`{"content":[{"type":"text","text":"..."}, {"type":"tool_call","name":"...","args":{...}}]}`，或者某种 Gemma 自己的风味。不管是哪种，**先把格式落到纸面上**再让 T3/T4 依赖它。

产出：本计划末尾追加一段说明实际观察到的 schema。临时 binary 丢 `/tmp`，**不**进 git。

提交信息（只提交那段附录）：`docs: P6 — document LiteRT-LM conversation response schema`

## Task 3：让 `LlmNode` 改用 Conversation

**文件：** `agentflow/nodes/llm_node.{h,cc}`

### Step 3.1：把 `BuildConversationJson` 和 `session.Start` 都换掉

- 从 `cfg_.system_prompt` 构造 `system_message_json`。
- 从 `cfg_.tool_registry->ExportToolsJson(cfg_.tool_names)` 构造 `tools_json`（没注册 registry 就传 `"[]"`）。
- `messages_json` 先传空（单次调用、没有先前历史）。从 `cfg_.input_field` 构造用户 `message_json`。
- `auto conv = LiteRtLmConversation::Create(engine, opts, io_ctx);`
- `conv->SendMessage(message_json);`
- 把 `NextTokenAsync()` 拉到 `accum`，每块 chunk 触发一次 `EmitToken`。
- 把 `conv->FullResponseJson()` 写到 `cfg_.output_field`（按 spec §5.1，LlmNode 就是单次调用节点；结构化的工具派发是 AgentNode 的事）。

要不要保留 `LiteRtLmSession` 旧路径？**不要**——单次调用的 LlmNode 用户同样会因为套上聊天模板而受益。Session API 留着给「我就是想丢裸 prompt」的高级用户。

### Step 3.2：提交

`refactor: LlmNode uses LiteRtLmConversation (chat template + tool schema applied by engine)`

## Task 4：让 `AgentNode` 在 ReAct 循环里改用 Conversation

**文件：** `agentflow/nodes/agent_node.{h,cc}`

### Step 4.1：生命周期变化

一个 Conversation 会跨多次消息存活；引擎自己维护历史。AgentNode 的 ReAct 循环就变成：

```cpp
auto conv = LiteRtLmConversation::Create(engine, /*system+tools+空历史*/, io_ctx);
conv->SendMessage(user_message_json);

for (int iter = 0; iter < cfg_.max_iter; ++iter) {
  std::string accum;
  while (true) {
    auto tok = co_await conv->NextTokenAsync();
    if (tok.empty()) break;
    accum += tok;
    if (cfg_.stream_tokens) emit.EmitToken(Id(), tok);
  }

  auto resp = json::parse(conv->FullResponseJson());
  // 具体 schema 见 T2 的落定。分支:
  if (resp 包含 tool_calls) {
    for (每个 call) {
      emit.EmitToolCall(Id(), name, args);
      auto result = co_await cfg_.tool_registry->Invoke(name, args, cancel);
      emit.EmitToolReturn(Id(), name, result);
      conv->SendMessage(MakeToolResponseJson(call_id, name, result));
    }
    continue;  // 引擎把工具响应追加进历史,然后继续 decode 下一轮
  } else {
    WriteOutput(state, accum_or_resp_text);
    break;
  }
}
```

### Step 4.2：删死代码

- `BuildConversationJson`（用不上了——引擎自己拼）。
- `ReadMessages`（历史归引擎管）。
- `AppendMessage`（同上）。
- `cfg_.messages_field` 变成空操作；用一行注释明确弃用：`// 保留位 —— P6 起，引擎自己管对话历史`。

### Step 4.3：提交

`refactor: AgentNode drives ReAct via Conversation; engine owns message history`

## Task 5：测试

### Step 5.1：用 `MODEL_PATH` 门控集成测试

`tests/unit/nodes/agent_node_test.cc`：
- 现有的 `DISABLED_SimpleResponse` 本来就需要 `MODEL_PATH`。把它**取消 DISABLE**；重命名成 `AgentNodeIntegrationTest.RealModelAnswersHello`（`MODEL_PATH` 没设的时候用 `GTEST_SKIP()` 跳过）。
- 新增 `AgentNodeIntegrationTest.UsesGetTimeTool`——注册 get_time 工具，prompt 问「现在几点」，断言：
  - assistant 回复非空
  - 至少触发了一次 `tool_name == "get_time"` 的 TOOL_CALL 事件
  - 回复里能匹配到一个可解析的时间子串（正则 `\d{4}` 命中年份或秒）。

`tests/unit/nodes/llm_node_test.cc`：
- 对称地加一个 `LlmNodeIntegrationTest.RealModelReturnsText`——单次 SendMessage，断言回复非空。

### Step 5.2：纯单元测试（不依赖模型）

`AgentNode` 现在已经不持有 message_json 的拼接逻辑了，所以没引擎的话就没法纯单测这一段。**这是可接受的取舍**：AgentNode 因此变薄了。原本不依赖模型的 ctor 校验之类的 AgentNode 单测照常存在。

### Step 5.3：提交

`test: gate Llm/Agent integration tests on MODEL_PATH; add get_time tool dispatch test`

## Task 6：Demo 验证

`examples/agent-demo/main.cc`：
- get_time 工具已经接好了。T4 之后用 `MODEL_PATH=$(realpath models/gemma-4-E2B-it.litertlm)` 重跑。
- 期望结果：assistant 回复里包含真实的时间字符串。trace 里能看到 `TOOL_CALL{tool_name=get_time}` → `TOOL_RETURN{result_json=<time>}` → `TOKEN` 一串 → 最终回答。
- 拿来跟 P5 时期的 `Assistant: ],"name":"assistant"}]}}` 对比，确认问题真的修了。

提交（main.cc 真的需要改才提）：`examples: agent-demo verified end-to-end with get_time tool`

## Task 7：打 tag + 开 PR

- `git tag p6-conversation`
- push 分支 + 对 master 开 PR。
- PR 标题：`feat(p6): use LiteRT-LM Conversation API for chat template + tool parsing`
- PR 正文列出关掉的两个 bug（没套用聊天模板；OpenAI 跟 Gemma 的 tool-call 形状不匹配）、新增的封装类、以及 demo 验过的输出。

提交：`chore: tag p6-conversation`

## 自审

**为什么不直接在 AgentNode 里手写模板拼接？**
那样就得在 C++ 里塞一个 jinja runtime，还得跟着模型一起维护模板。引擎已经在做这件事，并且把这份模板按模型存进 `.litertlm` 文件里。用它自己的 API 严格地更便宜。

**为什么不和 P5 合到一个 PR 里？**
P5 的范围是可观测性；P6 的范围是推理正确性。两者正交——正是因为 P5 那条 JSONL trace 落地了，我们才一眼看见了这个缺口。

**风险：**
- Conversation API 可能在我们链接进来的 `libce_*.a` 里没接全。T2 就是探针——`litert_lm_conversation_create` 要是返回 NULL 或段错误，计划停下，回退到 AgentNode 内部手工套模板（丑一些，但是可行）。
- 响应 JSON schema（T2 的产出）决定了 AgentNode 的解析逻辑。LiteRT-LM 以后版本里 schema 变了 AgentNode 就会断。LiteRT-LM 用 submodule SHA 固定（已经做了）。
- Conversation 自己持有历史；如果用户在两次迭代之间去改 `state.messages_field`，那些改动会被静默丢掉。T4 step 4.2 的「空操作」注释必须写得显眼。

**不在范围内（YAGNI）：**
- 多模态输入（`kInputImage`、`kInputAudio` 类型）。
- Conversation 持久化/检查点（如果真的需要，留给 P8 去管）。
- 流式的部分 tool call（引擎是整个 tool-call 对象一次性返回；流式只针对 assistant 文本）。

## 执行交接

继续做的时候，先读这份文档，然后从 Task 1 开始。每个 task 收尾要落一次 commit，不要攒着一起提。T2 的产出（响应 schema 那段附录）是 T3/T4 的解锁条件——schema 如果跟预期不同，先回过来改计划再动代码。
