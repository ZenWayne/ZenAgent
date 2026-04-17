# Edge Agent Service 架构设计文档 v2

**日期**: 2026-04-14
**主题**: 基于 Rig trait 体系的端侧 Agent 服务
**状态**: 待审核
**前序文档**: 2026-04-12-edge-agent-service-design.md（已废弃，本文档为完全重新设计）

---

## 1. 项目概述

### 1.1 目标

基于 LiteRT-LM（Google 端侧 LLM 推理框架）和 Rig（Rust LLM 应用框架），构建端侧 Agent 服务。核心是实现 Rig 的 trait 体系，让 LiteRT-LM 成为 Rig 的本地推理后端，与 Rig 生态无缝集成。

### 1.2 核心需求

1. **实现 Rig trait**：通过 C++ FFI 让 LiteRT-LM 实现 Rig 的 `CompletionModel` 和 `EmbeddingModel` trait
2. **聚合/解耦**：本地推理（LiteRT-LM FFI）和远程推理（Rig 内置 OpenAI/Ollama provider）通过配置切换，LLM 和 Embedding 可独立指定
3. **端侧 Embedding**：用 LiteRT 的 TFLite 运行时加载 encoder-only 模型（如 all-MiniLM-L6-v2.tflite）做文本向量化
4. **目标平台**：Android ARM64、嵌入式 Linux ARM64/ARMv7（树莓派等）
5. **交付形态**：library crate（`litert-sys` + `rig-litert`）+ 参考二进制（`edge-agent`）

### 1.3 与 v1 设计的根本区别

| 维度 | v1（2026-04-12） | v2（本文档） |
|------|-----------------|-------------|
| trait 体系 | 自建 `InferenceProvider` / `EmbeddingProvider` | 直接实现 Rig 的 `CompletionModel` / `EmbeddingModel` |
| Rig 集成深度 | 浅（仅概念参考） | 深（Agent builder、Tool、RAG pipeline 全部原生可用） |
| HTTP 服务端 | 需要（暴露 OpenAI 兼容 API） | 不需要（远程调用由 Rig 内置 provider 处理） |
| Embedding 方案 | 假设 LiteRT-LM Engine 支持 | 明确用 TFLite C API 加载独立 encoder 模型 |
| 架构层数 | 2 层（agent-layer + inference-layer） | 3 个 crate（litert-sys + rig-litert + edge-agent） |

---

## 2. 架构设计

### 2.1 整体结构

```
┌──────────────────────────────────────────────────────────────┐
│                     edge-agent (binary)                       │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  Config-driven provider selection                      │  │
│  │  - YAML 配置文件指定 local / remote                    │  │
│  │  - CLI 参数覆盖                                        │  │
│  │  - LLM 和 Embedding 独立指定 provider                  │  │
│  └────────────┬───────────────────────────────┬───────────┘  │
│               │                               │              │
│               ▼                               ▼              │
│  ┌─────────────────────┐         ┌──────────────────────┐   │
│  │  rig-litert          │         │  Rig built-in        │   │
│  │  (local provider)    │         │  providers           │   │
│  │                      │         │  (OpenAI/Ollama/...) │   │
│  │  CompletionModel ────┤         │  CompletionModel     │   │
│  │  EmbeddingModel  ────┤         │  EmbeddingModel      │   │
│  └──────────┬───────────┘         └──────────────────────┘   │
│             │                                                │
│             ▼                                                │
│  ┌─────────────────────┐                                    │
│  │  litert-sys          │                                    │
│  │  (C/C++ FFI)         │                                    │
│  │                      │                                    │
│  │  LlmEngine ──────────┤── LiteRT-LM Engine (LLM 推理)     │
│  │  TfLiteRunner ───────┤── LiteRT TFLite (Embedding 推理)  │
│  └──────────────────────┘                                    │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 关键设计决策

1. **LLM 和 Embedding 使用 LiteRT-LM 的不同层级**
   - LLM 推理：用 Engine C API（`c/engine.h`），走 prefill → decode → generate 完整流程
   - Embedding 推理：绕过 Engine，直接用底层 LiteRT TFLite C API 加载 encoder-only 模型，执行 tokenize → forward → pooling

2. **Provider 选择是纯应用层逻辑**
   - 配置文件决定 LLM 用本地还是远程，Embedding 用本地还是远程
   - 可以混搭（例如 LLM 远程 + Embedding 本地）
   - 不在 crate 层面做 dynamic dispatch 抽象

3. **Rig Agent 使用方式不变**
   - 无论底层是 `rig-litert` 还是 OpenAI provider，上层 Agent builder、Tool、RAG pipeline 代码完全一样
   - 这是语言层面的 trait 抽象保证，不是表面相似

### 2.3 Workspace 目录结构

```
edge-agent-workspace/
├── Cargo.toml                  # workspace 根
├── .cargo/
│   └── config.toml             # 交叉编译 target + linker 配置
├── litert-sys/                 # C/C++ FFI 绑定
│   ├── Cargo.toml
│   ├── build.rs                # bindgen + CMake 构建
│   ├── wrapper.h               # 引入 LiteRT-LM C API 头文件
│   └── src/
│       ├── lib.rs              # 模块入口
│       ├── raw.rs              # bindgen 生成（自动，不手写）
│       ├── engine.rs           # LlmEngine 安全封装
│       ├── tflite.rs           # TfLiteRunner 安全封装
│       └── error.rs            # FFI 错误类型
├── rig-litert/                 # Rig provider 实现
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs              # 公开 API 入口
│       ├── provider.rs         # LiteRTProvider 定义
│       ├── completion.rs       # impl CompletionModel
│       ├── embedding.rs        # impl EmbeddingModel
│       └── tokenizer.rs        # Embedding 分词
├── edge-agent/                 # 参考二进制
│   ├── Cargo.toml
│   └── src/
│       ├── main.rs             # CLI 入口 + REPL
│       └── config.rs           # 配置加载
├── LiteRT-LM/                  # git submodule
└── toolchains/                 # CMake toolchain files
    ├── android-arm64.cmake
    └── linux-arm64.cmake
```

---

## 3. `litert-sys` — C/C++ FFI 层

### 3.1 构建流程

`build.rs` 负责：
1. 用 `cmake` crate 构建 LiteRT-LM 的 C 库（输出 `liblitertlm.a`）
2. 根据 Rust 编译目标自动选择 CMake toolchain（Android NDK / ARM Linux）
3. 用 `bindgen` 从 `c/engine.h` 生成 Rust 原始绑定
4. 输出链接指令

```rust
// build.rs 核心逻辑

fn main() {
    let target = std::env::var("TARGET").unwrap();

    let mut cmake = cmake::Config::new("../LiteRT-LM");

    match target.as_str() {
        t if t.contains("android") => {
            let ndk = std::env::var("ANDROID_NDK_HOME")
                .expect("需要设置 ANDROID_NDK_HOME");
            cmake.define("CMAKE_TOOLCHAIN_FILE",
                format!("{ndk}/build/cmake/android.toolchain.cmake"));
            cmake.define("ANDROID_ABI", "arm64-v8a");
            cmake.define("ANDROID_PLATFORM", "android-28");
        }
        t if t.contains("aarch64") && t.contains("linux") => {
            cmake.define("CMAKE_TOOLCHAIN_FILE",
                "../toolchains/linux-arm64.cmake");
        }
        _ => {}
    }

    let dst = cmake.build();

    bindgen::Builder::default()
        .header("wrapper.h")
        .generate()
        .unwrap()
        .write_to_file(out_dir.join("bindings.rs"))
        .unwrap();

    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib=static=litertlm");
    println!("cargo:rustc-link-lib=dylib=stdc++");
}
```

### 3.2 `LlmEngine` — LLM 推理安全封装

```rust
// src/engine.rs

/// LiteRT-LM Engine 的安全 Rust 封装
pub struct LlmEngine {
    ptr: NonNull<ffi::LiteRtLmEngine>,
}

unsafe impl Send for LlmEngine {}

impl LlmEngine {
    /// 加载模型，创建引擎
    pub fn new(config: &EngineConfig) -> Result<Self, LiteRtError>;

    /// 创建推理会话（持有 KV cache 状态）
    pub fn create_session(&self) -> Result<LlmSession, LiteRtError>;
}

pub struct LlmSession {
    ptr: NonNull<ffi::LiteRtLmSession>,
    _engine: Arc<LlmEngine>,  // 保持 engine 存活
}

impl LlmSession {
    /// 阻塞式生成
    pub fn generate(&mut self, prompt: &str, opts: &GenerateOpts) -> Result<String, LiteRtError>;

    /// 流式生成，通过回调逐 token 返回
    pub fn generate_stream(
        &mut self,
        prompt: &str,
        opts: &GenerateOpts,
        callback: impl FnMut(&str) -> bool,  // 返回 false 中止
    ) -> Result<String, LiteRtError>;
}

pub struct EngineConfig {
    pub model_path: PathBuf,
    pub backend: Backend,
    pub num_threads: usize,
}

pub enum Backend {
    Cpu,
    Gpu,
    Npu,
}

pub struct GenerateOpts {
    pub max_tokens: usize,
    pub temperature: f32,
    pub top_k: Option<usize>,
    pub top_p: Option<f32>,
}
```

**`&mut self` 的含义**：LlmSession 的推理会修改内部状态（KV cache），所以要求独占访问。Rust 编译器保证同一时间只有一个调用者能使用 session，避免多线程数据竞争。上层用 `Mutex` 包装来满足并发需求。

### 3.3 `TfLiteRunner` — Embedding 推理安全封装

```rust
// src/tflite.rs

/// 通用 TFLite 模型运行器，用于 encoder-only embedding 模型
pub struct TfLiteRunner {
    interpreter: NonNull<ffi::TfLiteInterpreter>,
    input_dims: Vec<usize>,
    output_dims: Vec<usize>,
}

unsafe impl Send for TfLiteRunner {}

impl TfLiteRunner {
    /// 加载 .tflite embedding 模型
    pub fn new(model_path: &Path, num_threads: usize) -> Result<Self, LiteRtError>;

    /// 执行推理：输入 token IDs → 输出 float 向量
    pub fn invoke(&mut self, input: &[i32]) -> Result<Vec<f32>, LiteRtError>;

    /// 输出维度
    pub fn output_dims(&self) -> &[usize];
}
```

### 3.4 错误类型

```rust
// src/error.rs

#[derive(Debug, thiserror::Error)]
pub enum LiteRtError {
    #[error("model load failed: {0}")]
    ModelLoad(String),

    #[error("session creation failed: {0}")]
    SessionCreate(String),

    #[error("inference failed: {0}")]
    Inference(String),

    #[error("invalid model format: expected {expected}, got {actual}")]
    InvalidFormat { expected: String, actual: String },

    #[error("backend not available: {0}")]
    BackendUnavailable(String),
}
```

---

## 4. `rig-litert` — Rig Provider 层

### 4.1 Provider 定义

```rust
// src/provider.rs

/// LiteRT 本地 Provider —— 不走 HTTP，直接调 C FFI
pub struct LiteRTProvider {
    engine: Arc<Mutex<LlmSession>>,
    runner: Arc<Mutex<TfLiteRunner>>,
    tokenizer: Arc<EmbeddingTokenizer>,
    config: LiteRTConfig,
}

pub struct LiteRTConfig {
    pub llm_model_path: PathBuf,
    pub embedding_model_path: PathBuf,
    pub embedding_tokenizer_path: PathBuf,  // HuggingFace tokenizer.json
    pub backend: Backend,
    pub num_threads: usize,
    pub embedding_dims: usize,
}

impl LiteRTProvider {
    pub fn new(config: LiteRTConfig) -> Result<Self, LiteRtError> {
        // 1. 创建 LLM Engine → Session
        // 2. 创建 TfLite Runner（加载 embedding 模型）
        // 3. 加载 embedding tokenizer
    }

    /// 获取 CompletionModel（给 Rig Agent 用）
    pub fn completion_model(&self, model_name: &str) -> LiteRTCompletionModel;

    /// 获取 EmbeddingModel（给 Rig RAG pipeline 用）
    pub fn embedding_model(&self, model_name: &str) -> LiteRTEmbeddingModel;
}
```

### 4.2 实现 `CompletionModel`

```rust
// src/completion.rs

#[derive(Clone)]
pub struct LiteRTCompletionModel {
    session: Arc<Mutex<LlmSession>>,
    model_name: String,
}

impl CompletionModel for LiteRTCompletionModel {
    type Response = LiteRTResponse;
    type StreamingResponse = LiteRTStreamChunk;
    type Client = LiteRTProvider;

    fn make(client: &Self::Client, model: impl Into<String>) -> Self {
        // 从 provider 获取 session 的 Arc 引用
    }

    async fn completion(&self, request: CompletionRequest) -> Result<...> {
        // 1. CompletionRequest → prompt 文本
        //    拼接 messages（system + user + history）
        //    提取 tools 定义 → 注入 prompt（如果有）
        // 2. 拿 Mutex 锁，调用 session.generate()
        // 3. 解析输出：纯文本 or tool_call
        // 4. 转换成 Rig CompletionResponse 返回
    }

    async fn stream(&self, request: CompletionRequest) -> Result<...> {
        // 类似 completion，用 session.generate_stream()
        // 通过 tokio channel 把回调转成 async Stream
    }
}
```

**Prompt 组装**：端侧模型接收纯文本而非结构化 messages 数组，需要按模型的 chat template 拼接（如 Gemma 格式）。LiteRT-LM Engine 内部已处理 chat template，将 messages 按顺序传入即可。

### 4.3 实现 `EmbeddingModel`

```rust
// src/embedding.rs

#[derive(Clone)]
pub struct LiteRTEmbeddingModel {
    runner: Arc<Mutex<TfLiteRunner>>,
    tokenizer: Arc<EmbeddingTokenizer>,
    dims: usize,
    model_name: String,
}

impl EmbeddingModel for LiteRTEmbeddingModel {
    const MAX_DOCUMENTS: usize = 64;
    type Client = LiteRTProvider;

    fn ndims(&self) -> usize {
        self.dims
    }

    fn make(client: &Self::Client, model: impl Into<String>, dims: Option<usize>) -> Self {
        // 从 provider 获取 runner + tokenizer 的 Arc 引用
    }

    async fn embed_texts(
        &self,
        texts: impl IntoIterator<Item = String>,
    ) -> Result<Vec<Embedding>, EmbeddingError> {
        let texts: Vec<String> = texts.into_iter().collect();
        let mut results = Vec::with_capacity(texts.len());
        for text in &texts {
            // 1. 分词: "hello world" → [101, 7592, 2088, 102]
            let token_ids = self.tokenizer.encode(text);
            // 2. 推理: token_ids → raw float 输出
            let mut runner = self.runner.lock().await;
            let raw_output = runner.invoke(&token_ids)?;
            // 3. Mean pooling + L2 归一化 → 最终向量
            let embedding = mean_pool_and_normalize(&raw_output, self.dims);
            results.push(Embedding::from(embedding));
        }
        Ok(results)
    }
}
```

**Embedding 推理流水线**：
```
"hello world"
     │
     ▼ tokenizer.encode()
[101, 7592, 2088, 102]
     │
     ▼ TfLiteRunner::invoke()
[[0.12, -0.34, ...],       ← 每个 token 一个向量
 [0.56,  0.78, ...],
 [0.23, -0.11, ...],
 [0.45,  0.67, ...]]
     │
     ▼ mean_pool_and_normalize()
[0.34, 0.25, ...]          ← 最终: 一个固定维度的向量
```

### 4.4 使用示例

```rust
use rig_litert::LiteRTProvider;

// 创建本地 provider
let provider = LiteRTProvider::new(LiteRTConfig {
    llm_model_path: "models/gemma-2b.litertlm".into(),
    embedding_model_path: "models/all-MiniLM-L6-v2.tflite".into(),
    embedding_tokenizer_path: "models/tokenizer.json".into(),
    backend: Backend::Gpu,
    num_threads: 4,
    embedding_dims: 384,
})?;

// 用法和 OpenAI provider 完全一样
let agent = provider.completion_model("gemma-2b")
    .agent("You are a helpful assistant on an edge device.")
    .tool(my_tool)
    .build();

let response = agent.prompt("你好").await?;

// Embedding 也一样
let embedder = provider.embedding_model("all-MiniLM-L6-v2");
let vectors = embedder.embed_texts(vec!["hello".into()]).await?;
```

---

## 5. `edge-agent` — 参考二进制

### 5.1 配置文件

```yaml
# edge-agent.yaml

llm:
  provider: local                    # local | openai | ollama
  local:
    model_path: models/gemma-2b.litertlm
    backend: gpu                     # cpu | gpu | npu
    num_threads: 4
  openai:
    base_url: https://api.openai.com/v1
    api_key: ${OPENAI_API_KEY}
    model: gpt-4o
  ollama:
    base_url: http://192.168.1.100:11434
    model: llama3

embedding:
  provider: local                    # 可以跟 llm 独立配置
  local:
    model_path: models/all-MiniLM-L6-v2.tflite
    tokenizer_path: models/tokenizer.json
    dims: 384
  openai:
    model: text-embedding-3-small

agent:
  system_prompt: "你是一个运行在边缘设备上的助手。"
  temperature: 0.7
  max_tokens: 2048
```

`llm.provider` 和 `embedding.provider` 独立配置，支持混搭：
- 全本地：离线场景
- 全远程：算力不够时
- 混搭：LLM 走远程 GPU 服务器 + Embedding 本地小模型

### 5.2 核心逻辑

**注意**：Rig 的 `CompletionModel` 和 `EmbeddingModel` trait 有关联类型，不能直接用 `Box<dyn ...>`。需要用 enum 包装来实现运行时多态。

```rust
// src/main.rs

/// 用 enum 包装不同 provider 的 model，实现运行时选择
/// （因为 Rig 的 CompletionModel trait 有关联类型，不能用 Box<dyn ...>）
enum AnyAgent {
    LiteRT(Agent<LiteRTCompletionModel>),
    OpenAI(Agent<openai::CompletionModel>),
    Ollama(Agent<ollama::CompletionModel>),
}

impl AnyAgent {
    async fn prompt(&self, input: &str) -> Result<String> {
        match self {
            Self::LiteRT(a) => a.prompt(input).await,
            Self::OpenAI(a) => a.prompt(input).await,
            Self::Ollama(a) => a.prompt(input).await,
        }
    }
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let config = Config::load()?;

    // 根据配置构建对应 provider 的 Agent
    let agent: AnyAgent = match config.llm.provider {
        Provider::Local => {
            let p = LiteRTProvider::new(config.llm.local.into())?;
            let a = p.completion_model("local")
                .agent(&config.agent.system_prompt)
                .build();
            AnyAgent::LiteRT(a)
        }
        Provider::OpenAI => {
            let client = rig::providers::openai::Client::from_env();
            let a = client.completion_model(&config.llm.openai.model)
                .agent(&config.agent.system_prompt)
                .build();
            AnyAgent::OpenAI(a)
        }
        Provider::Ollama => {
            let client = rig::providers::ollama::Client::new(/*...*/);
            let a = client.completion_model(&config.llm.ollama.model)
                .agent(&config.agent.system_prompt)
                .build();
            AnyAgent::Ollama(a)
        }
    };

    // REPL 交互循环 —— 这里不关心底层是什么 provider
    loop {
        let input = read_line(">>> ")?;
        if input == "quit" { break; }
        let response = agent.prompt(&input).await?;
        println!("{}", response);
    }

    Ok(())
}
```

### 5.3 CLI 参数

```bash
# 默认配置
edge-agent

# 指定配置文件
edge-agent --config path/to/config.yaml

# CLI 覆盖
edge-agent --llm-provider openai --embedding-provider local

# 单次问答（非交互）
edge-agent --once "今天天气怎么样"
```

### 5.4 数据流

```
用户输入 "帮我总结这篇文章"
    │
    ▼
  Agent (Rig)
    │
    ├──→ 需要 RAG？→ EmbeddingModel.embed_texts("帮我总结这篇文章")
    │         │           │
    │         │     ┌─────┴─────────────────────────┐
    │         │     │ local: TfLiteRunner FFI        │
    │         │     │ openai: HTTP → api.openai.com  │
    │         │     └─────┬─────────────────────────┘
    │         │           ▼
    │         │    VectorStore.top_n(query_vec)
    │         │           ▼
    │         │    检索到相关文档片段
    │         │           │
    │    ◄────┘    注入到 prompt 上下文
    │
    ▼
  CompletionModel.completion(request + context)
    │           │
    │     ┌─────┴─────────────────────────┐
    │     │ local: LlmEngine C++ FFI      │
    │     │ openai: HTTP → api.openai.com │
    │     │ ollama: HTTP → ollama server   │
    │     └─────┬─────────────────────────┘
    │           ▼
    │    模型回复（可能包含 tool_call）
    │           │
    ├──→ 有 tool_call？→ 执行工具 → 结果送回模型 → 再次生成
    │
    ▼
  最终回复给用户
```

---

## 6. 构建与交叉编译

### 6.1 交叉编译配置

```toml
# .cargo/config.toml

[target.aarch64-linux-android]
linker = "aarch64-linux-android34-clang++"

[target.aarch64-unknown-linux-gnu]
linker = "aarch64-linux-gnu-gcc"
```

### 6.2 构建命令

```bash
# 本机（开发调试）
cargo build

# Android ARM64
export ANDROID_NDK_HOME=/path/to/ndk
cargo build --target aarch64-linux-android

# 树莓派 / 嵌入式 Linux ARM64
cargo build --target aarch64-unknown-linux-gnu
```

### 6.3 LiteRT-LM 引入方式

以 git submodule 引入 LiteRT-LM 源码，`litert-sys/build.rs` 驱动 CMake 构建。交叉编译时 `build.rs` 根据 Rust 编译目标自动选择 CMake toolchain file。

---

## 7. 依赖总览

| Crate | 依赖 | 说明 |
|-------|------|------|
| `litert-sys` | `cmake`, `bindgen` (build-deps) | 构建时生成 C/C++ 绑定 |
| `rig-litert` | `litert-sys`, `rig-core`, `tokio`, `tokenizers` | `tokenizers` = HuggingFace Rust 分词器 |
| `edge-agent` | `rig-litert`, `rig-core`, `clap`, `serde_yaml`, `anyhow`, `tokio` | CLI + 配置 + 异步运行时 |

---

## 8. 实现阶段

1. **Phase 1**: 创建 workspace + git submodule + 基础构建验证
2. **Phase 2**: `litert-sys` — bindgen 生成绑定 + `LlmEngine` 安全封装 + 本机测试
3. **Phase 3**: `litert-sys` — `TfLiteRunner` 封装 + embedding 模型加载测试
4. **Phase 4**: `rig-litert` — 实现 `CompletionModel` trait + 单轮对话验证
5. **Phase 5**: `rig-litert` — 实现 `EmbeddingModel` trait + 向量化验证
6. **Phase 6**: `rig-litert` — 流式响应 + tool calling 集成
7. **Phase 7**: `edge-agent` — 配置系统 + CLI + REPL
8. **Phase 8**: 交叉编译 Android ARM64 + Linux ARM64 验证
9. **Phase 9**: 集成测试 + 示例文档

---

## 9. 相关链接

- LiteRT-LM: https://github.com/google-ai-edge/LiteRT-LM
- Rig: https://github.com/0xplaygrounds/rig
- Rig CompletionModel trait: `rig-core/src/completion/request.rs`
- Rig EmbeddingModel trait: `rig-core/src/embeddings/embedding.rs`
- LiteRT-LM C API: `LiteRT-LM/c/engine.h`
- 前序设计文档: `docs/superpowers/specs/2026-04-12-edge-agent-service-design.md`
