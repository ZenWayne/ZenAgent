# Edge Agent Service 架构设计文档

**日期**: 2026-04-12  
**主题**: 端侧 Agent 服务分层架构  
**状态**: 已批准，待实现

---

## 1. 项目概述

### 1.1 目标

构建一个可解耦也可聚合的一站式端侧 Agent 服务，基于以下两个核心组件：

- **LiteRT-LM**: Google 的边缘设备 LLM 推理框架
- **Rig**: Rust LLM 应用框架，提供统一的 Provider 抽象

### 1.2 核心需求

1. **双模式部署**: 聚合模式（单二进制）和解耦模式（HTTP 服务）
2. **运行时切换**: 支持本地/远程推理的动态切换和自动 fallback
3. **OpenAI 兼容**: 远程调用遵循 OpenAI API 标准
4. **基础 Agent 能力**: 多轮对话、工具调用、流式响应
5. **端侧 Embedding**: 文本向量化，支持本地和远程模型

---

## 2. 架构设计

### 2.1 整体结构

```
┌─────────────────────────────────────────────────────────────────┐
│                         Agent Layer                              │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐ │
│  │  Agent      │───►│ Tool Router │    │  Conversation       │ │
│  │  (对话管理)  │◄───│ (函数调用)   │    │  (上下文管理)        │ │
│  └──────┬──────┘    └─────────────┘    └─────────────────────┘ │
│         │                                                       │
│         │  CompletionRequest { prompt, tools, config }         │
│         ▼                                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │           InferenceProvider (trait)                      │   │
│  │  ┌─────────────┐              ┌─────────────────────┐   │   │
│  │  │ LiteRTLocal │              │ OpenAICompatible    │   │   │
│  │  │  (本地推理)  │              │ (HTTP远程调用)       │   │   │
│  │  └──────┬──────┘              └──────────┬──────────┘   │   │
│  └─────────┼────────────────────────────────┼──────────────┘   │
│            │                                │                  │
│            │                                │                  │
│            ▼                                ▼                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │          EmbeddingProvider (trait)                       │   │
│  │  ┌─────────────┐              ┌─────────────────────┐   │   │
│  │  │ LiteRTEmbed │              │ OpenAIEmbed         │   │   │
│  │  │  (本地嵌入)  │              │ (HTTP远程嵌入)       │   │   │
│  │  └──────┬──────┘              └──────────┬──────────┘   │   │
│  └─────────┼────────────────────────────────┼──────────────┘   │
└────────────┼────────────────────────────────┼──────────────────┘
             │                                │
             │                                │
             ▼                                ▼
┌─────────────────────────┐      ┌─────────────────────────────┐
│    Inference Layer       │      │    External Service          │
│  ┌─────────────────┐    │      │  (OpenAI/Ollama/vLLM/etc)   │
│  │ LiteRT Session  │    │      │                             │
│  │ - LLM 推理       │    │      │  POST /v1/chat/completions  │
│  │ - Embedding     │    │      │  POST /v1/embeddings        │
│  │ - Token生成     │    │      │                             │
│  └─────────────────┘    │      └─────────────────────────────┘
└─────────────────────────┘
```

### 2.2 目录结构

```
edge-agent-service/
├── Cargo.toml              # workspace 定义
├── agent-layer/            # Agent层 crate
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs
│       ├── agent.rs        # Agent 定义
│       ├── config.rs       # 配置管理
│       ├── error.rs        # 错误类型
│       ├── provider/       # Provider 抽象
│       │   ├── mod.rs      # InferenceProvider + EmbeddingProvider trait
│       │   ├── litert.rs   # LiteRT 本地实现 (LLM + Embedding)
│       │   ├── remote.rs   # OpenAI兼容远程实现
│       │   └── manager.rs  # Provider 管理器
│       ├── embedding.rs    # Embedding 客户端
│       └── tools/          # 工具调用
│           └── mod.rs
├── inference-layer/        # 推理层 crate
│   ├── Cargo.toml
│   └── src/
│       ├── lib.rs
│       ├── session.rs      # LiteRT 会话管理
│       └── model.rs        # 模型加载/配置
└── examples/
    ├── embedded.rs         # 聚合模式示例
    └── remote_client.rs    # 解耦模式客户端示例
```

---

## 3. 核心组件设计

### 3.1 Provider Trait 抽象

```rust
// agent-layer/src/provider/mod.rs

#[async_trait]
pub trait InferenceProvider: Send + Sync {
    /// 同步完成
    async fn complete(&self, request: CompletionRequest) -> Result<CompletionResponse>;
    
    /// 流式完成
    async fn stream_complete(&self, request: CompletionRequest) 
        -> Result<BoxStream<'static, Result<StreamChunk>>>;
    
    /// 获取 provider 信息
    fn info(&self) -> ProviderInfo;
    
    /// 健康检查
    async fn health_check(&self) -> Result<()>;
}

pub struct ProviderInfo {
    pub name: String,
    pub supports_streaming: bool,
    pub context_window: usize,
}

/// Embedding Provider Trait
#[async_trait]
pub trait EmbeddingProvider: Send + Sync {
    /// 对单个文本进行嵌入
    async fn embed(&self, text: &str) -> Result<Embedding>;
    
    /// 批量嵌入（更高效）
    async fn embed_batch(&self, texts: &[&str]) -> Result<Vec<Embedding>>;
    
    /// 获取嵌入维度
    fn dimensions(&self) -> usize;
    
    /// 获取 provider 信息
    fn info(&self) -> EmbeddingProviderInfo;
}

pub struct EmbeddingProviderInfo {
    pub name: String,
    pub dimensions: usize,
    pub max_input_length: usize,
}

/// 嵌入向量
pub type Embedding = Vec<f32>;
```

### 3.2 Agent 实现

```rust
// agent-layer/src/agent.rs

pub struct Agent {
    provider_manager: Arc<ProviderManager>,
    conversation: Conversation,
    tools: ToolRegistry,
    config: AgentConfig,
}

pub struct AgentConfig {
    pub system_prompt: String,
    pub temperature: f32,
    pub max_tokens: Option<usize>,
    pub enable_tools: bool,
}

impl Agent {
    /// 单轮对话
    pub async fn chat(&self, message: &str) -> Result<String>;
    
    /// 流式对话
    pub async fn chat_stream(&self, message: &str) -> Result<impl Stream<Item = Result<String>>>;
    
    /// 带工具调用的对话
    pub async fn chat_with_tools(&self, message: &str) -> Result<String>;
    
    /// 带自动 fallback 的对话
    pub async fn chat_with_fallback(&self, message: &str) -> Result<String>;
}
```

### 3.3 LiteRT 推理层

```rust
// inference-layer/src/lib.rs

pub struct LiteRTSession {
    model: Arc<Model>,
    tokenizer: Tokenizer,
    config: RuntimeConfig,
}

pub struct RuntimeConfig {
    pub num_threads: usize,
    pub use_gpu: bool,
    pub cache_dir: PathBuf,
}

impl LiteRTSession {
    /// 加载模型
    pub fn new(model_path: impl AsRef<Path>) -> Result<Self>;
    
    /// 执行推理
    pub async fn generate(&self, prompt: &str, options: GenerateOptions) -> Result<String>;
}

// 实现 InferenceProvider trait
#[async_trait]
impl InferenceProvider for LiteRTSession {
    // ...
}
```

### 3.4 Provider 管理器

```rust
// agent-layer/src/provider/manager.rs

pub struct ProviderManager {
    config: AgentServiceConfig,
    current: Arc<RwLock<Box<dyn InferenceProvider>>>,
    cache: ProviderCache,
}

impl ProviderManager {
    /// 创建管理器
    pub async fn new(config: AgentServiceConfig) -> Result<Self>;
    
    /// 切换 provider（运行时）
    pub async fn switch(&self, new_type: ProviderType) -> Result<()>;
    
    /// 获取当前 provider
    pub async fn current(&self) -> Arc<dyn InferenceProvider>;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ProviderType {
    Local,
    Remote,
}
```

---

## 4. 数据模型

### 4.1 核心数据结构

```rust
// 共享类型定义

pub struct CompletionRequest {
    pub messages: Vec<Message>,
    pub tools: Option<Vec<ToolDefinition>>,
    pub stream: bool,
    pub temperature: f32,
    pub max_tokens: Option<usize>,
}

pub struct CompletionResponse {
    pub content: String,
    pub tool_calls: Option<Vec<ToolCall>>,
}

pub struct Message {
    pub role: Role,
    pub content: String,
}

pub enum Role {
    System,
    User,
    Assistant,
}

pub struct ToolDefinition {
    pub name: String,
    pub description: String,
    pub parameters: serde_json::Value, // JSON Schema
}

pub struct ToolCall {
    pub id: String,
    pub name: String,
    pub arguments: serde_json::Value,
}

/// Embedding 请求
pub struct EmbeddingRequest {
    pub input: EmbeddingInput,
    pub model: Option<String>,
}

pub enum EmbeddingInput {
    Single(String),
    Multiple(Vec<String>),
}

/// Embedding 响应
pub struct EmbeddingResponse {
    pub embeddings: Vec<Embedding>,
    pub model: String,
    pub usage: TokenUsage,
}

pub struct TokenUsage {
    pub prompt_tokens: usize,
    pub total_tokens: usize,
}

/// 文本片段（带向量的文档块）
pub struct TextChunk {
    pub id: String,
    pub content: String,
    pub embedding: Option<Embedding>,
    pub metadata: HashMap<String, String>,
}
```

---

## 5. 错误处理

### 5.1 错误类型层次

```rust
// agent-layer/src/error.rs

#[derive(Error, Debug)]
pub enum AgentError {
    #[error("inference failed: {0}")]
    Inference(#[from] InferenceError),
    
    #[error("embedding failed: {0}")]
    Embedding(#[from] EmbeddingError),
    
    #[error("tool execution failed: {name} - {message}")]
    ToolExecution { name: String, message: String },
    
    #[error("context window exceeded: {current}/{max} tokens")]
    ContextWindowExceeded { current: usize, max: usize },
    
    #[error("provider unavailable: {0}")]
    ProviderUnavailable(String),
    
    #[error("invalid configuration: {0}")]
    Config(String),
}

#[derive(Error, Debug)]
pub enum InferenceError {
    #[error("model load failed: {0}")]
    ModelLoad(String),
    
    #[error("inference timeout")]
    Timeout,
    
    #[error("remote service error: {status} - {message}")]
    RemoteService { status: u16, message: String },
    
    #[error("local engine error: {0}")]
    LocalEngine(String),
}

#[derive(Error, Debug)]
pub enum EmbeddingError {
    #[error("embedding model load failed: {0}")]
    ModelLoad(String),
    
    #[error("input too long: {length} > {max}")]
    InputTooLong { length: usize, max: usize },
    
    #[error("batch size exceeded: {size} > {max}")]
    BatchSizeExceeded { size: usize, max: usize },
    
    #[error("remote service error: {status} - {message}")]
    RemoteService { status: u16, message: String },
    
    #[error("local engine error: {0}")]
    LocalEngine(String),
}
```

---

## 6. 配置管理

### 6.1 配置结构

```rust
// agent-layer/src/config.rs

#[derive(Debug, Clone, Deserialize)]
pub struct AgentServiceConfig {
    /// 默认 LLM provider 类型
    pub default_provider: ProviderType,
    /// 默认 Embedding provider 类型（不指定则使用 LLM 相同的）
    pub default_embedding_provider: Option<ProviderType>,
    pub local: Option<LocalConfig>,
    pub remote: Option<RemoteConfig>,
    pub agent: AgentDefaults,
    pub embedding: EmbeddingDefaults,
}

#[derive(Debug, Clone, Deserialize)]
pub struct LocalConfig {
    pub model_path: PathBuf,
    pub num_threads: usize,
    pub use_gpu: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct RemoteConfig {
    pub base_url: String,
    pub api_key: Option<String>,
    pub timeout_secs: u64,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AgentDefaults {
    pub temperature: f32,
    pub max_tokens: usize,
    pub system_prompt: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct EmbeddingDefaults {
    pub model: String,
    pub dimensions: usize,
    pub max_input_length: usize,
}
```

### 6.2 配置文件示例

```yaml
# edge-agent.yaml

default_provider: local
default_embedding_provider: local  # 可选，不设置则使用 default_provider

local:
  # LLM 模型
  model_path: ./models/gemma-2b.it.tflite
  # Embedding 模型（可选，支持独立配置）
  embedding_model_path: ./models/all-MiniLM-L6-v2.tflite
  num_threads: 4
  use_gpu: true

remote:
  base_url: http://localhost:8080/v1
  api_key: null
  timeout_secs: 30

agent:
  temperature: 0.7
  max_tokens: 2048
  system_prompt: "You are a helpful assistant running on an edge device."

embedding:
  model: "text-embedding-3-small"
  dimensions: 1536
  max_input_length: 8192
```

---

## 7. 使用模式

### 7.1 聚合模式（嵌入式）

```rust
// examples/embedded.rs

use agent_layer::{Agent, AgentConfig};
use inference_layer::LiteRTSession;

#[tokio::main]
async fn main() -> Result<()> {
    // 创建本地推理会话
    let session = LiteRTSession::new("gemma-2b.it.tflite")?;
    
    // Agent 直接使用本地推理
    let agent = Agent::builder()
        .provider(session)
        .config(AgentConfig {
            system_prompt: "You are a helpful assistant.".into(),
            temperature: 0.7,
            max_tokens: Some(2048),
            enable_tools: true,
        })
        .build();
    
    let resp = agent.chat("Hello").await?;
    println!("{}", resp);
    
    Ok(())
}
```

### 7.2 解耦模式（HTTP 客户端）

```rust
// examples/remote_client.rs

use agent_layer::{Agent, AgentConfig};
use agent_layer::provider::OpenAiCompatibleClient;

#[tokio::main]
async fn main() -> Result<()> {
    // 连接远程推理服务
    let client = OpenAiCompatibleClient::new("http://localhost:8080/v1");
    
    let agent = Agent::builder()
        .provider(client)
        .config(AgentConfig::default())
        .build();
    
    let resp = agent.chat("Hello").await?;
    println!("{}", resp);
    
    Ok(())
}
```

### 7.3 运行时切换与 Fallback

```rust
// 使用 ProviderManager 进行动态切换

let manager = ProviderManager::new(config).await?;

// 获取当前 provider
let agent = Agent::builder()
    .provider_manager(manager.clone())
    .build();

// 手动切换到远程
manager.switch(ProviderType::Remote).await?;

// 或让 Agent 自动处理 fallback
let resp = agent.chat_with_fallback("Hello").await?;
```

### 7.4 Embedding 使用

```rust
// 使用独立的 EmbeddingProvider

use agent_layer::embedding::EmbeddingClient;
use agent_layer::provider::EmbeddingProvider;

#[tokio::main]
async fn main() -> Result<()> {
    let config = AgentServiceConfig::load()?;
    let provider = ProviderManager::new(config).await?;
    
    // 获取 embedding provider（可以是本地 LiteRT 或远程 OpenAI）
    let embedder = provider.embedding_provider().await?;
    
    // 单文本嵌入
    let embedding = embedder.embed("Hello, world!").await?;
    println!("Dimensions: {}", embedding.len());
    
    // 批量嵌入（更高效）
    let texts = vec!["First text", "Second text", "Third text"];
    let embeddings = embedder.embed_batch(&texts).await?;
    
    Ok(())
}
```

### 7.5 RAG 简单示例

```rust
// 基础 RAG：文档向量化 + 相似度检索

use agent_layer::embedding::{EmbeddingClient, TextChunk};

struct SimpleRAG {
    embedder: Arc<dyn EmbeddingProvider>,
    documents: Vec<TextChunk>,
}

impl SimpleRAG {
    async fn add_document(&mut self, content: &str) -> Result<()> {
        let embedding = self.embedder.embed(content).await?;
        self.documents.push(TextChunk {
            id: uuid::Uuid::new_v4().to_string(),
            content: content.to_string(),
            embedding: Some(embedding),
            metadata: HashMap::new(),
        });
        Ok(())
    }
    
    fn search(&self, query: &Embedding, top_k: usize) -> Vec<&TextChunk> {
        // 余弦相似度计算
        let mut scored: Vec<_> = self.documents.iter()
            .filter_map(|doc| {
                doc.embedding.as_ref().map(|emb| {
                    let score = cosine_similarity(query, emb);
                    (doc, score)
                })
            })
            .collect();
        
        scored.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap());
        scored.into_iter().take(top_k).map(|(doc, _)| doc).collect()
    }
}
```

---

## 8. 依赖关系

| Crate | 主要依赖 | 说明 |
|-------|---------|------|
| `agent-layer` | `rig-core`, `tokio`, `serde`, `thiserror`, `reqwest` | 提供 Agent 逻辑和 Provider 抽象 |
| `inference-layer` | `litert` (绑定), `tokenizers`, `ndarray` | 封装 LiteRT 推理能力 |

---

## 9. 实现计划

1. **Phase 1**: 创建 workspace 结构和基础类型定义
2. **Phase 2**: 实现 `InferenceProvider` trait 和 OpenAI 兼容客户端
3. **Phase 3**: 实现 `EmbeddingProvider` trait 和远程嵌入客户端
4. **Phase 4**: 集成 LiteRT，实现本地 LLM + Embedding provider
5. **Phase 5**: 实现 Agent 层（对话管理、工具调用）
6. **Phase 6**: 实现 ProviderManager（支持 LLM 和 Embedding 独立配置）
7. **Phase 7**: 实现 EmbeddingClient 和基础 RAG 工具
8. **Phase 8**: 编写示例和测试

---

## 10. 附录

### 10.1 相关链接

- LiteRT-LM: https://github.com/google-ai-edge/LiteRT-LM
- Rig: https://github.com/0xplaygrounds/rig
