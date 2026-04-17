# Edge Agent Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Rust workspace with three crates (`litert-sys`, `rig-litert`, `edge-agent`) that makes LiteRT-LM a native Rig provider for on-device LLM inference and text embedding.

**Architecture:** `litert-sys` wraps LiteRT-LM's C API via bindgen into safe Rust types. `rig-litert` implements Rig's `CompletionModel` and `EmbeddingModel` traits on top of those wrappers. `edge-agent` is a config-driven reference binary that selects local (LiteRT) or remote (OpenAI/Ollama) providers at startup.

**Tech Stack:** Rust, rig-core 0.35, LiteRT-LM C API (via bindgen), TFLite C API, tokio, HuggingFace tokenizers, clap, serde_yaml

**Spec:** `docs/superpowers/specs/2026-04-14-edge-agent-service-design.md`

**Spec deviation:** The plan uses LiteRT-LM's Conversation API (`litert_lm_conversation_*`) instead of the raw Session API. The Conversation API natively handles system messages, tool definitions (JSON), chat templates, and multi-turn context — a much better fit for Rig's `CompletionModel` than manual prompt assembly.

---

## File Map

```
edge-agent-workspace/
├── Cargo.toml                      # workspace root
├── .cargo/config.toml              # cross-compilation linker settings
├── LiteRT-LM/                      # git submodule
├── toolchains/
│   ├── android-arm64.cmake         # Android NDK toolchain
│   └── linux-arm64.cmake           # RPi / embedded Linux toolchain
│
├── litert-sys/
│   ├── Cargo.toml                  # deps: thiserror; build-deps: cmake, bindgen
│   ├── build.rs                    # CMake build + bindgen generation
│   ├── wrapper.h                   # #include "c/engine.h"
│   └── src/
│       ├── lib.rs                  # re-exports
│       ├── error.rs                # LiteRtError enum
│       ├── engine.rs               # LlmEngine, LlmConversation wrappers
│       └── tflite.rs               # TfLiteRunner wrapper
│
├── rig-litert/
│   ├── Cargo.toml                  # deps: litert-sys, rig-core, tokio, tokenizers, serde, serde_json, thiserror
│   └── src/
│       ├── lib.rs                  # re-exports
│       ├── error.rs                # RigLiteRtError, conversions to CompletionError/EmbeddingError
│       ├── provider.rs             # LiteRTProvider: owns engine + runner + tokenizer
│       ├── completion.rs           # LiteRTCompletionModel: impl CompletionModel
│       ├── streaming.rs            # LiteRTStreamingResponse, callback→channel bridge
│       ├── embedding.rs            # LiteRTEmbeddingModel: impl EmbeddingModel
│       ├── tokenizer_wrapper.rs    # EmbeddingTokenizer: wraps HuggingFace tokenizers
│       └── convert.rs              # CompletionRequest → JSON, JSON → CompletionResponse
│
├── edge-agent/
│   ├── Cargo.toml                  # deps: rig-litert, rig-core, clap, serde_yaml, serde, anyhow, tokio
│   └── src/
│       ├── main.rs                 # CLI entrypoint + REPL loop
│       └── config.rs               # Config struct + YAML loading
│
└── tests/
    └── integration/
        └── smoke.rs                # End-to-end: load model → prompt → get response
```

---

### Task 1: Workspace Scaffold + Git Submodule

**Files:**
- Create: `Cargo.toml` (workspace root)
- Create: `litert-sys/Cargo.toml`
- Create: `litert-sys/src/lib.rs`
- Create: `rig-litert/Cargo.toml`
- Create: `rig-litert/src/lib.rs`
- Create: `edge-agent/Cargo.toml`
- Create: `edge-agent/src/main.rs`
- Create: `.cargo/config.toml`
- Create: `toolchains/android-arm64.cmake`
- Create: `toolchains/linux-arm64.cmake`

- [ ] **Step 1: Initialize git repo and add LiteRT-LM submodule**

```bash
cd /home/wayne/tools/zen/brainstorm_never_go_in_this_folder
mkdir edge-agent-workspace && cd edge-agent-workspace
git init
git submodule add https://github.com/google-ai-edge/LiteRT-LM.git LiteRT-LM
```

- [ ] **Step 2: Create workspace root Cargo.toml**

```toml
# Cargo.toml
[workspace]
resolver = "2"
members = [
    "litert-sys",
    "rig-litert",
    "edge-agent",
]
```

- [ ] **Step 3: Create litert-sys crate skeleton**

```toml
# litert-sys/Cargo.toml
[package]
name = "litert-sys"
version = "0.1.0"
edition = "2021"

[dependencies]
thiserror = "2"

[build-dependencies]
cmake = "0.1"
bindgen = "0.71"
```

```rust
// litert-sys/src/lib.rs
pub mod error;
```

```rust
// litert-sys/src/error.rs
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

    #[error("null pointer returned from C API: {0}")]
    NullPointer(String),
}
```

- [ ] **Step 4: Create rig-litert crate skeleton**

```toml
# rig-litert/Cargo.toml
[package]
name = "rig-litert"
version = "0.1.0"
edition = "2021"

[dependencies]
litert-sys = { path = "../litert-sys" }
rig-core = { git = "https://github.com/0xplaygrounds/rig.git", package = "rig-core" }
tokio = { version = "1", features = ["sync", "rt"] }
tokenizers = "0.21"
serde = { version = "1", features = ["derive"] }
serde_json = "1"
thiserror = "2"
async-stream = "0.3"
futures = "0.3"
tracing = "0.1"
```

```rust
// rig-litert/src/lib.rs
pub mod error;
```

```rust
// rig-litert/src/error.rs
use litert_sys::LiteRtError;

#[derive(Debug, thiserror::Error)]
pub enum RigLiteRtError {
    #[error("litert engine error: {0}")]
    Engine(#[from] LiteRtError),

    #[error("tokenizer error: {0}")]
    Tokenizer(String),

    #[error("json conversion error: {0}")]
    Json(#[from] serde_json::Error),
}
```

- [ ] **Step 5: Create edge-agent crate skeleton**

```toml
# edge-agent/Cargo.toml
[package]
name = "edge-agent"
version = "0.1.0"
edition = "2021"

[dependencies]
rig-litert = { path = "../rig-litert" }
rig-core = { git = "https://github.com/0xplaygrounds/rig.git", package = "rig-core" }
tokio = { version = "1", features = ["full"] }
clap = { version = "4", features = ["derive"] }
serde = { version = "1", features = ["derive"] }
serde_yaml = "0.9"
anyhow = "1"
```

```rust
// edge-agent/src/main.rs
fn main() {
    println!("edge-agent placeholder");
}
```

- [ ] **Step 6: Create cross-compilation configs**

```toml
# .cargo/config.toml
[target.aarch64-linux-android]
linker = "aarch64-linux-android34-clang++"

[target.aarch64-unknown-linux-gnu]
linker = "aarch64-linux-gnu-gcc"

[target.armv7-unknown-linux-gnueabihf]
linker = "arm-linux-gnueabihf-gcc"
```

```cmake
# toolchains/android-arm64.cmake
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 28)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(CMAKE_ANDROID_NDK $ENV{ANDROID_NDK_HOME})
```

```cmake
# toolchains/linux-arm64.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
```

- [ ] **Step 7: Verify workspace compiles**

Run: `cargo check`
Expected: All three crates compile successfully (no LiteRT-LM C build yet — litert-sys has no build.rs yet).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat: scaffold workspace with litert-sys, rig-litert, edge-agent crates"
```

---

### Task 2: litert-sys — build.rs + Bindgen

**Files:**
- Create: `litert-sys/build.rs`
- Create: `litert-sys/wrapper.h`

**Prerequisite:** LiteRT-LM submodule checked out, CMake installed. On host machine (not cross-compiling yet).

- [ ] **Step 1: Create the wrapper header**

```c
// litert-sys/wrapper.h
#include "../LiteRT-LM/c/engine.h"
```

- [ ] **Step 2: Write build.rs**

```rust
// litert-sys/build.rs
use std::env;
use std::path::PathBuf;

fn main() {
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());
    let target = env::var("TARGET").unwrap();
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let litert_lm_dir = manifest_dir.join("..").join("LiteRT-LM");

    // --- Step 1: Build LiteRT-LM C library via CMake ---
    let mut cmake_config = cmake::Config::new(&litert_lm_dir);
    cmake_config
        .define("LITERT_LM_BUILD_C_API", "ON")
        .define("CMAKE_BUILD_TYPE", "Release");

    // Cross-compilation: select toolchain based on Rust target
    match target.as_str() {
        t if t.contains("android") => {
            let ndk = env::var("ANDROID_NDK_HOME")
                .expect("ANDROID_NDK_HOME must be set for Android builds");
            cmake_config.define(
                "CMAKE_TOOLCHAIN_FILE",
                format!("{ndk}/build/cmake/android.toolchain.cmake"),
            );
            cmake_config.define("ANDROID_ABI", "arm64-v8a");
            cmake_config.define("ANDROID_PLATFORM", "android-28");
        }
        t if t.contains("aarch64") && t.contains("linux") && !t.contains("android") => {
            let toolchain = manifest_dir.join("..").join("toolchains").join("linux-arm64.cmake");
            cmake_config.define("CMAKE_TOOLCHAIN_FILE", toolchain.to_str().unwrap());
        }
        _ => {
            // Host build — no special toolchain needed
        }
    }

    let dst = cmake_config.build();

    // --- Step 2: Generate Rust bindings via bindgen ---
    let bindings = bindgen::Builder::default()
        .header("wrapper.h")
        .clang_arg(format!("-I{}", litert_lm_dir.display()))
        .allowlist_function("litert_lm_.*")
        .allowlist_type("LiteRtLm.*")
        .allowlist_type("InputData.*")
        .allowlist_type("Type")
        .allowlist_var("k.*")
        .generate()
        .expect("Failed to generate bindings");

    bindings
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("Failed to write bindings");

    // --- Step 3: Link instructions ---
    println!("cargo:rustc-link-search=native={}/lib", dst.display());
    println!("cargo:rustc-link-lib=static=litertlm");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rerun-if-changed=wrapper.h");
}
```

- [ ] **Step 3: Expose raw bindings in lib.rs**

```rust
// litert-sys/src/lib.rs
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

/// Raw FFI bindings generated by bindgen from LiteRT-LM's `c/engine.h`.
pub mod ffi {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

pub mod error;
pub mod engine;
pub mod tflite;

pub use error::LiteRtError;
pub use engine::{LlmEngine, LlmConversation, EngineConfig, ConversationConfig, Backend};
pub use tflite::TfLiteRunner;
```

- [ ] **Step 4: Attempt build to verify bindgen works**

Run: `cargo build -p litert-sys 2>&1 | head -50`
Expected: Either succeeds (if CMake build of LiteRT-LM works out of the box), or fails with a CMake error about missing flags/deps. If it fails, diagnose and fix the CMake invocation. Common issues: missing `LITERT_LM_BUILD_C_API` flag (check LiteRT-LM's CMakeLists.txt for the actual flag name), missing submodule deps.

**Note:** If the CMake build requires additional configuration (LiteRT-LM's build system is complex), adjust `build.rs` accordingly. The exact CMake defines depend on LiteRT-LM's build system, which may need `LITERT_LM_BUILD_SHARED_LIBS=OFF` or similar. Check `LiteRT-LM/CMakeLists.txt` and `LiteRT-LM/cmake/` for available options.

- [ ] **Step 5: Commit**

```bash
git add litert-sys/build.rs litert-sys/wrapper.h litert-sys/src/lib.rs
git commit -m "feat(litert-sys): add build.rs with CMake + bindgen integration"
```

---

### Task 3: litert-sys — LlmEngine + LlmConversation Safe Wrappers

**Files:**
- Create: `litert-sys/src/engine.rs`

**Reference:** LiteRT-LM C API at `LiteRT-LM/c/engine.h` — uses opaque pointers with `_create` / `_delete` lifecycle.

- [ ] **Step 1: Write engine.rs with all wrapper types**

```rust
// litert-sys/src/engine.rs
use crate::error::LiteRtError;
use crate::ffi;
use std::ffi::CString;
use std::path::Path;
use std::ptr::NonNull;
use std::sync::Arc;

/// Which hardware accelerator to use.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Backend {
    Cpu,
    Gpu,
    Npu,
}

impl Backend {
    fn as_c_str(&self) -> &str {
        match self {
            Backend::Cpu => "cpu",
            Backend::Gpu => "gpu",
            Backend::Npu => "npu",
        }
    }
}

/// Configuration for creating an LLM engine.
#[derive(Debug, Clone)]
pub struct EngineConfig {
    pub model_path: std::path::PathBuf,
    pub backend: Backend,
    pub max_tokens: Option<i32>,
    pub cache_dir: Option<std::path::PathBuf>,
}

/// Configuration for creating a conversation.
#[derive(Debug, Clone)]
pub struct ConversationConfig {
    pub system_message_json: Option<String>,
    pub tools_json: Option<String>,
    pub max_output_tokens: Option<i32>,
    pub temperature: Option<f32>,
    pub top_k: Option<i32>,
    pub top_p: Option<f32>,
}

/// Safe wrapper around the LiteRT-LM engine.
/// One engine per loaded model. Create conversations from it.
pub struct LlmEngine {
    ptr: NonNull<ffi::LiteRtLmEngine>,
}

// LiteRT-LM's engine is thread-safe for creating conversations.
unsafe impl Send for LlmEngine {}
unsafe impl Sync for LlmEngine {}

impl LlmEngine {
    /// Load a model and create the engine.
    pub fn new(config: &EngineConfig) -> Result<Self, LiteRtError> {
        let model_path = CString::new(
            config.model_path.to_str()
                .ok_or_else(|| LiteRtError::ModelLoad("invalid model path".into()))?
        ).map_err(|e| LiteRtError::ModelLoad(e.to_string()))?;

        let backend = CString::new(config.backend.as_c_str())
            .map_err(|e| LiteRtError::ModelLoad(e.to_string()))?;

        unsafe {
            let settings = ffi::litert_lm_engine_settings_create(
                model_path.as_ptr(),
                backend.as_ptr(),
                std::ptr::null(), // vision_backend
                std::ptr::null(), // audio_backend
            );
            if settings.is_null() {
                return Err(LiteRtError::ModelLoad("failed to create engine settings".into()));
            }

            if let Some(max_tokens) = config.max_tokens {
                ffi::litert_lm_engine_settings_set_max_num_tokens(settings, max_tokens);
            }
            if let Some(ref cache_dir) = config.cache_dir {
                let dir = CString::new(cache_dir.to_str().unwrap_or_default())
                    .map_err(|e| LiteRtError::ModelLoad(e.to_string()))?;
                ffi::litert_lm_engine_settings_set_cache_dir(settings, dir.as_ptr());
            }

            let engine = ffi::litert_lm_engine_create(settings);
            ffi::litert_lm_engine_settings_delete(settings);

            NonNull::new(engine)
                .map(|ptr| LlmEngine { ptr })
                .ok_or_else(|| LiteRtError::ModelLoad("engine creation returned null".into()))
        }
    }

    /// Raw pointer — used internally by LlmConversation.
    fn as_ptr(&self) -> *mut ffi::LiteRtLmEngine {
        self.ptr.as_ptr()
    }
}

impl Drop for LlmEngine {
    fn drop(&mut self) {
        unsafe {
            ffi::litert_lm_engine_delete(self.ptr.as_ptr());
        }
    }
}

/// A multi-turn conversation backed by LiteRT-LM.
/// Handles system prompt, tool definitions, and chat template formatting internally.
///
/// Requires `&mut self` for send_message because the conversation has internal state.
pub struct LlmConversation {
    ptr: NonNull<ffi::LiteRtLmConversation>,
    _engine: Arc<LlmEngine>, // prevent engine from being dropped
}

unsafe impl Send for LlmConversation {}

impl LlmConversation {
    /// Create a new conversation from an engine.
    pub fn new(engine: Arc<LlmEngine>, config: &ConversationConfig) -> Result<Self, LiteRtError> {
        unsafe {
            // Build session config for sampler parameters
            let session_config = ffi::litert_lm_session_config_create();
            if session_config.is_null() {
                return Err(LiteRtError::SessionCreate("failed to create session config".into()));
            }

            if let Some(max_output) = config.max_output_tokens {
                ffi::litert_lm_session_config_set_max_output_tokens(session_config, max_output);
            }

            // Set sampler params if any temperature/top_k/top_p specified
            if config.temperature.is_some() || config.top_k.is_some() || config.top_p.is_some() {
                let sampler = ffi::LiteRtLmSamplerParams {
                    type_: if config.top_p.is_some() {
                        ffi::Type_kTopP
                    } else if config.top_k.is_some() {
                        ffi::Type_kTopK
                    } else {
                        ffi::Type_kTopK // default
                    },
                    top_k: config.top_k.unwrap_or(40),
                    top_p: config.top_p.unwrap_or(0.95),
                    temperature: config.temperature.unwrap_or(0.7),
                    seed: 0,
                };
                ffi::litert_lm_session_config_set_sampler_params(session_config, &sampler);
            }

            // Build conversation config
            let system_msg = config.system_message_json.as_deref()
                .map(|s| CString::new(s).unwrap());
            let tools = config.tools_json.as_deref()
                .map(|s| CString::new(s).unwrap());

            let conv_config = ffi::litert_lm_conversation_config_create(
                engine.as_ptr(),
                session_config,
                system_msg.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                tools.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                std::ptr::null(), // messages_json (start fresh)
                tools.is_some(),  // enable constrained decoding when tools are present
            );
            ffi::litert_lm_session_config_delete(session_config);

            if conv_config.is_null() {
                return Err(LiteRtError::SessionCreate("failed to create conversation config".into()));
            }

            let conv = ffi::litert_lm_conversation_create(engine.as_ptr(), conv_config);
            ffi::litert_lm_conversation_config_delete(conv_config);

            NonNull::new(conv)
                .map(|ptr| LlmConversation { ptr, _engine: engine })
                .ok_or_else(|| LiteRtError::SessionCreate("conversation creation returned null".into()))
        }
    }

    /// Send a message and get a blocking response. Returns the response JSON string.
    pub fn send_message(&mut self, message_json: &str, extra_context: Option<&str>) -> Result<String, LiteRtError> {
        let msg = CString::new(message_json)
            .map_err(|e| LiteRtError::Inference(e.to_string()))?;
        let ctx = extra_context.map(|s| CString::new(s).unwrap());

        unsafe {
            let response = ffi::litert_lm_conversation_send_message(
                self.ptr.as_ptr(),
                msg.as_ptr(),
                ctx.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            );
            if response.is_null() {
                return Err(LiteRtError::Inference("send_message returned null".into()));
            }

            let json_ptr = ffi::litert_lm_json_response_get_string(response);
            let result = if json_ptr.is_null() {
                Err(LiteRtError::Inference("response json string is null".into()))
            } else {
                let s = std::ffi::CStr::from_ptr(json_ptr).to_string_lossy().into_owned();
                Ok(s)
            };

            ffi::litert_lm_json_response_delete(response);
            result
        }
    }

    /// Send a message with streaming response via callback.
    /// `on_chunk` receives each text chunk. Return `false` from it to cancel.
    /// The callback is invoked from a background thread by LiteRT-LM.
    pub fn send_message_stream<F>(
        &mut self,
        message_json: &str,
        extra_context: Option<&str>,
        on_chunk: F,
    ) -> Result<(), LiteRtError>
    where
        F: FnMut(&str, bool) + Send + 'static,
    {
        let msg = CString::new(message_json)
            .map_err(|e| LiteRtError::Inference(e.to_string()))?;
        let ctx = extra_context.map(|s| CString::new(s).unwrap());

        // Box the closure so we can pass a raw pointer to C
        let callback_data = Box::into_raw(Box::new(on_chunk));

        unsafe extern "C" fn stream_callback<F: FnMut(&str, bool) + Send>(
            callback_data: *mut std::ffi::c_void,
            chunk: *const std::ffi::c_char,
            is_final: bool,
            error_msg: *const std::ffi::c_char,
        ) where
            F: FnMut(&str, bool) + Send,
        {
            let f = &mut *(callback_data as *mut F);
            if !error_msg.is_null() {
                // Error occurred — pass empty chunk with is_final=true
                f("", true);
                return;
            }
            let text = if chunk.is_null() {
                ""
            } else {
                std::ffi::CStr::from_ptr(chunk).to_str().unwrap_or("")
            };
            f(text, is_final);
        }

        unsafe {
            let ret = ffi::litert_lm_conversation_send_message_stream(
                self.ptr.as_ptr(),
                msg.as_ptr(),
                ctx.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                Some(stream_callback::<F>),
                callback_data as *mut std::ffi::c_void,
            );

            // Note: callback_data ownership is tricky. The callback will be invoked
            // from a background thread. We need to ensure it's cleaned up after
            // is_final=true. For now we leak it — Task 7 (streaming) addresses proper cleanup.
            if ret != 0 {
                // Streaming failed to start — clean up
                let _ = Box::from_raw(callback_data);
                return Err(LiteRtError::Inference(format!("stream failed to start: error code {ret}")));
            }

            Ok(())
        }
    }
}

impl Drop for LlmConversation {
    fn drop(&mut self) {
        unsafe {
            ffi::litert_lm_conversation_delete(self.ptr.as_ptr());
        }
    }
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cargo check -p litert-sys`
Expected: Compiles (or fails at link time if LiteRT-LM C library isn't built yet — that's OK at this stage, the types and logic should pass type-checking).

- [ ] **Step 3: Commit**

```bash
git add litert-sys/src/engine.rs litert-sys/src/lib.rs
git commit -m "feat(litert-sys): add LlmEngine and LlmConversation safe wrappers"
```

---

### Task 4: litert-sys — TfLiteRunner Wrapper (for Embedding Models)

**Files:**
- Create: `litert-sys/src/tflite.rs`
- Modify: `litert-sys/build.rs` (add TFLite C API bindings)
- Modify: `litert-sys/wrapper.h` (add TFLite header)

**Context:** Embedding models (e.g., all-MiniLM-L6-v2) are encoder-only transformer models saved as `.tflite` files. Unlike LLM inference which uses LiteRT-LM's Engine/Conversation API, we run these directly through the TFLite C interpreter API: load model → allocate tensors → set input → invoke → read output.

- [ ] **Step 1: Add TFLite C API header to wrapper.h**

```c
// litert-sys/wrapper.h
#include "../LiteRT-LM/c/engine.h"

// TFLite C API for running embedding models directly
// The header location depends on how LiteRT-LM bundles TFLite.
// Common locations:
//   - LiteRT-LM/third_party/tflite/c/c_api.h
//   - LiteRT-LM/external/tflite/c/c_api.h
// If not bundled, install TFLite separately and point to its headers.
// Uncomment the correct path after verifying:
// #include "tensorflow/lite/c/c_api.h"
```

**Note to implementer:** Run `find LiteRT-LM/ -name "c_api.h" -path "*/lite/*"` to locate the TFLite C API header. If LiteRT-LM doesn't bundle TFLite headers directly, you'll need to either:
- Build TFLite from LiteRT-LM's dependencies, or
- Use the system-installed TFLite package

- [ ] **Step 2: Write tflite.rs**

```rust
// litert-sys/src/tflite.rs
use crate::error::LiteRtError;
use std::ffi::CString;
use std::path::Path;
use std::ptr::NonNull;

// These are TFLite C API types. The exact bindgen names depend on the header.
// Placeholder names — adjust after bindgen output is inspected.
// The TFLite C API functions follow this pattern:
//   TfLiteInterpreterCreate(model, options) → *TfLiteInterpreter
//   TfLiteInterpreterAllocateTensors(interp)
//   TfLiteInterpreterGetInputTensor(interp, index) → *TfLiteTensor
//   TfLiteTensorCopyFromBuffer(tensor, data, size)
//   TfLiteInterpreterInvoke(interp)
//   TfLiteInterpreterGetOutputTensor(interp, index) → *TfLiteTensor
//   TfLiteTensorCopyToBuffer(tensor, data, size)

/// Runs a TFLite model for embedding inference.
/// Wraps the TFLite C interpreter API.
pub struct TfLiteRunner {
    // The exact internal fields depend on TFLite C API availability.
    // If TFLite C API is not directly available through LiteRT-LM,
    // this wrapper will need adaptation.
    //
    // For now, define the interface. The implementation will be filled in
    // once we confirm how TFLite is bundled with LiteRT-LM.
    model_path: std::path::PathBuf,
    num_threads: usize,
    output_dims: Vec<usize>,
    // interpreter: NonNull<ffi::TfLiteInterpreter>,  // uncomment after bindgen
}

unsafe impl Send for TfLiteRunner {}

impl TfLiteRunner {
    /// Load a .tflite embedding model.
    ///
    /// Implementation requires TFLite C API headers. The steps are:
    /// TfLiteModelCreateFromFile → TfLiteInterpreterOptionsCreate (set threads) →
    /// TfLiteInterpreterCreate → AllocateTensors → read output shape.
    ///
    /// Returns an error until TFLite C API integration is wired in build.rs.
    pub fn new(model_path: &Path, num_threads: usize) -> Result<Self, LiteRtError> {
        Err(LiteRtError::ModelLoad(
            "TfLiteRunner::new not yet implemented — waiting for TFLite C API integration".into()
        ))
    }

    /// Run inference: input token IDs → output float vector.
    ///
    /// Input: token IDs from the embedding model's tokenizer (e.g., [101, 7592, 2088, 102]).
    /// Output: raw model output — typically shape [1, seq_len, hidden_dim] or [1, hidden_dim].
    /// The caller (rig-litert's embedding.rs) is responsible for pooling and normalization.
    ///
    /// Implementation: resize input tensor → copy input_ids → invoke → copy output buffer.
    /// Returns an error until TFLite C API integration is wired in build.rs.
    pub fn invoke(&mut self, input_ids: &[i32]) -> Result<Vec<f32>, LiteRtError> {
        Err(LiteRtError::Inference(
            "TfLiteRunner::invoke not yet implemented — waiting for TFLite C API integration".into()
        ))
    }

    /// The shape of the output tensor (e.g., [1, 384] for MiniLM).
    pub fn output_dims(&self) -> &[usize] {
        &self.output_dims
    }
}
```

**Note:** TfLiteRunner has a placeholder implementation because the exact TFLite C API integration depends on how LiteRT-LM bundles TFLite. The interface is defined so `rig-litert` can program against it. The implementation will be filled in during integration testing (Task 4 is about establishing the interface; the wiring happens when a real .tflite model is available for testing).

- [ ] **Step 3: Verify it compiles**

Run: `cargo check -p litert-sys`
Expected: Compiles.

- [ ] **Step 4: Commit**

```bash
git add litert-sys/src/tflite.rs litert-sys/wrapper.h litert-sys/src/lib.rs
git commit -m "feat(litert-sys): add TfLiteRunner interface for embedding model inference"
```

---

### Task 5: rig-litert — Error Conversions + Provider Struct

**Files:**
- Modify: `rig-litert/src/error.rs` (add conversions to Rig error types)
- Create: `rig-litert/src/provider.rs`
- Modify: `rig-litert/src/lib.rs`

- [ ] **Step 1: Add Rig error conversions to error.rs**

```rust
// rig-litert/src/error.rs
use litert_sys::LiteRtError;
use rig_core::completion::CompletionError;
use rig_core::embeddings::EmbeddingError;

#[derive(Debug, thiserror::Error)]
pub enum RigLiteRtError {
    #[error("litert engine error: {0}")]
    Engine(#[from] LiteRtError),

    #[error("tokenizer error: {0}")]
    Tokenizer(String),

    #[error("json conversion error: {0}")]
    Json(#[from] serde_json::Error),

    #[error("response parse error: {0}")]
    ResponseParse(String),
}

impl From<RigLiteRtError> for CompletionError {
    fn from(e: RigLiteRtError) -> Self {
        CompletionError::ProviderError(e.to_string())
    }
}

impl From<RigLiteRtError> for EmbeddingError {
    fn from(e: RigLiteRtError) -> Self {
        EmbeddingError::ProviderError(e.to_string())
    }
}
```

- [ ] **Step 2: Create provider.rs**

```rust
// rig-litert/src/provider.rs
use litert_sys::{Backend, EngineConfig, LlmConversation, LlmEngine, TfLiteRunner};
use std::path::PathBuf;
use std::sync::Arc;
use tokio::sync::Mutex;

use crate::completion::LiteRTCompletionModel;
use crate::embedding::LiteRTEmbeddingModel;
use crate::error::RigLiteRtError;
use crate::tokenizer_wrapper::EmbeddingTokenizer;

/// Configuration for creating a LiteRT provider.
#[derive(Debug, Clone)]
pub struct LiteRTConfig {
    /// Path to the LLM model file (.litertlm or .tflite).
    pub llm_model_path: PathBuf,
    /// Path to the embedding model file (.tflite).
    pub embedding_model_path: PathBuf,
    /// Path to the embedding tokenizer file (tokenizer.json from HuggingFace).
    pub embedding_tokenizer_path: PathBuf,
    /// Hardware backend for LLM inference.
    pub backend: Backend,
    /// Number of threads for TFLite embedding inference.
    pub embedding_num_threads: usize,
    /// Embedding vector dimensions (e.g., 384 for MiniLM).
    pub embedding_dims: usize,
    /// Maximum context tokens for LLM.
    pub max_tokens: Option<i32>,
}

/// A Rig-compatible provider backed by LiteRT-LM for local on-device inference.
///
/// Owns the LLM engine and embedding runner. Create `CompletionModel` and
/// `EmbeddingModel` instances from it — they share the underlying resources via Arc.
pub struct LiteRTProvider {
    engine: Arc<LlmEngine>,
    runner: Arc<Mutex<TfLiteRunner>>,
    tokenizer: Arc<EmbeddingTokenizer>,
    config: LiteRTConfig,
}

impl LiteRTProvider {
    /// Create a new provider. Loads the LLM model and embedding model.
    pub fn new(config: LiteRTConfig) -> Result<Self, RigLiteRtError> {
        // Load LLM engine
        let engine_config = EngineConfig {
            model_path: config.llm_model_path.clone(),
            backend: config.backend,
            max_tokens: config.max_tokens,
            cache_dir: None,
        };
        let engine = Arc::new(LlmEngine::new(&engine_config)?);

        // Load embedding TFLite runner
        let runner = TfLiteRunner::new(&config.embedding_model_path, config.embedding_num_threads)?;
        let runner = Arc::new(Mutex::new(runner));

        // Load embedding tokenizer
        let tokenizer = EmbeddingTokenizer::from_file(&config.embedding_tokenizer_path)
            .map_err(|e| RigLiteRtError::Tokenizer(e.to_string()))?;
        let tokenizer = Arc::new(tokenizer);

        Ok(Self { engine, runner, tokenizer, config })
    }

    /// Create a completion model for use with Rig's Agent builder.
    pub fn completion_model(&self, model_name: &str) -> LiteRTCompletionModel {
        LiteRTCompletionModel::new(self.engine.clone(), model_name.to_string())
    }

    /// Create an embedding model for use with Rig's RAG pipeline.
    pub fn embedding_model(&self, model_name: &str) -> LiteRTEmbeddingModel {
        LiteRTEmbeddingModel::new(
            self.runner.clone(),
            self.tokenizer.clone(),
            self.config.embedding_dims,
            model_name.to_string(),
        )
    }
}
```

- [ ] **Step 3: Update lib.rs**

```rust
// rig-litert/src/lib.rs
pub mod error;
pub mod provider;
pub mod completion;
pub mod streaming;
pub mod embedding;
pub mod tokenizer_wrapper;
pub mod convert;

pub use provider::{LiteRTProvider, LiteRTConfig};
pub use completion::LiteRTCompletionModel;
pub use embedding::LiteRTEmbeddingModel;
```

- [ ] **Step 4: Create placeholder modules so lib.rs compiles**

```rust
// rig-litert/src/completion.rs
use litert_sys::LlmEngine;
use std::sync::Arc;

#[derive(Clone)]
pub struct LiteRTCompletionModel {
    pub(crate) engine: Arc<LlmEngine>,
    pub(crate) model_name: String,
}

impl LiteRTCompletionModel {
    pub fn new(engine: Arc<LlmEngine>, model_name: String) -> Self {
        Self { engine, model_name }
    }
}
```

```rust
// rig-litert/src/streaming.rs
// Streaming support — implemented in Task 7
```

```rust
// rig-litert/src/embedding.rs
use litert_sys::TfLiteRunner;
use crate::tokenizer_wrapper::EmbeddingTokenizer;
use std::sync::Arc;
use tokio::sync::Mutex;

#[derive(Clone)]
pub struct LiteRTEmbeddingModel {
    pub(crate) runner: Arc<Mutex<TfLiteRunner>>,
    pub(crate) tokenizer: Arc<EmbeddingTokenizer>,
    pub(crate) dims: usize,
    pub(crate) model_name: String,
}

impl LiteRTEmbeddingModel {
    pub fn new(
        runner: Arc<Mutex<TfLiteRunner>>,
        tokenizer: Arc<EmbeddingTokenizer>,
        dims: usize,
        model_name: String,
    ) -> Self {
        Self { runner, tokenizer, dims, model_name }
    }
}
```

```rust
// rig-litert/src/tokenizer_wrapper.rs
use std::path::Path;

/// Wraps HuggingFace's `tokenizers` crate for embedding text tokenization.
pub struct EmbeddingTokenizer {
    inner: tokenizers::Tokenizer,
}

impl EmbeddingTokenizer {
    /// Load a tokenizer from a tokenizer.json file.
    pub fn from_file(path: &Path) -> Result<Self, String> {
        let inner = tokenizers::Tokenizer::from_file(path)
            .map_err(|e| e.to_string())?;
        Ok(Self { inner })
    }

    /// Encode text into token IDs.
    pub fn encode(&self, text: &str) -> Vec<i32> {
        self.inner
            .encode(text, true)
            .map(|enc| enc.get_ids().iter().map(|&id| id as i32).collect())
            .unwrap_or_default()
    }
}
```

```rust
// rig-litert/src/convert.rs
// CompletionRequest ↔ JSON conversions — implemented in Task 6
```

- [ ] **Step 5: Verify it compiles**

Run: `cargo check -p rig-litert`
Expected: Compiles (may need adjusting imports based on exact rig-core API version).

- [ ] **Step 6: Commit**

```bash
git add rig-litert/
git commit -m "feat(rig-litert): add provider struct, error conversions, and module skeletons"
```

---

### Task 6: rig-litert — Implement CompletionModel (Non-Streaming)

**Files:**
- Modify: `rig-litert/src/completion.rs`
- Modify: `rig-litert/src/convert.rs`

**Reference:** Rig's `CompletionModel` trait at `rig-core/src/completion/request.rs:466-501`. Ollama implementation at `rig-core/src/providers/ollama.rs:513-600`.

- [ ] **Step 1: Write convert.rs — CompletionRequest → JSON for LiteRT-LM Conversation API**

```rust
// rig-litert/src/convert.rs
use rig_core::completion::CompletionRequest;
use rig_core::completion::message::{AssistantContent, Message};
use rig_core::tool::ToolDefinition;
use rig_core::OneOrMany;
use serde_json::{json, Value};

/// Convert Rig's CompletionRequest into the JSON strings that
/// LiteRT-LM's Conversation API expects.
pub struct ConvertedRequest {
    /// The user message JSON string for `litert_lm_conversation_send_message`.
    pub message_json: String,
    /// System message JSON (for conversation creation, not per-message).
    pub system_message_json: Option<String>,
    /// Tools JSON array (for conversation creation).
    pub tools_json: Option<String>,
    /// Extra context from RAG documents.
    pub extra_context: Option<String>,
}

/// Extract the system message from the request.
pub fn extract_system_message(request: &CompletionRequest) -> Option<String> {
    // Rig puts the system prompt in request.preamble or as a System message in chat_history
    if let Some(ref preamble) = request.preamble {
        return Some(json!({ "role": "system", "content": preamble }).to_string());
    }

    // Check first message in history for System role
    for msg in request.chat_history.iter() {
        if let Message::System { content } = msg {
            let text = content.iter()
                .map(|c| c.to_string())
                .collect::<Vec<_>>()
                .join("\n");
            return Some(json!({ "role": "system", "content": text }).to_string());
        }
    }
    None
}

/// Convert Rig tool definitions to JSON array for LiteRT-LM.
pub fn tools_to_json(tools: &[ToolDefinition]) -> Option<String> {
    if tools.is_empty() {
        return None;
    }
    let tools_json: Vec<Value> = tools.iter().map(|t| {
        json!({
            "type": "function",
            "function": {
                "name": t.name,
                "description": t.description,
                "parameters": t.parameters
            }
        })
    }).collect();
    Some(serde_json::to_string(&tools_json).unwrap_or_default())
}

/// Convert Rig's chat history into a single user message JSON for the conversation.
/// The Conversation API manages multi-turn internally, so we send only the latest user message.
pub fn build_message_json(request: &CompletionRequest) -> String {
    // The last message in chat_history is always the user prompt
    let last_msg = request.chat_history.last();

    let content = match last_msg {
        Message::User { content } => {
            content.iter()
                .map(|c| c.to_string())
                .collect::<Vec<_>>()
                .join("\n")
        }
        _ => String::new(),
    };

    // Include documents as extra context if present
    let docs_text = if !request.documents.is_empty() {
        let docs: Vec<String> = request.documents.iter()
            .map(|d| d.to_string())
            .collect();
        Some(docs.join("\n\n"))
    } else {
        None
    };

    json!({
        "role": "user",
        "content": content
    }).to_string()
}

/// Build extra_context string from documents (for RAG).
pub fn build_extra_context(request: &CompletionRequest) -> Option<String> {
    if request.documents.is_empty() {
        return None;
    }
    let docs: Vec<String> = request.documents.iter()
        .map(|d| d.to_string())
        .collect();
    Some(json!({ "context": docs.join("\n\n") }).to_string())
}

/// Parse LiteRT-LM's JSON response into Rig's completion types.
pub fn parse_response_text(response_json: &str) -> Result<String, crate::error::RigLiteRtError> {
    // LiteRT-LM conversation response is a JSON string.
    // The exact format depends on the model, but typically contains a "content" field.
    let v: Value = serde_json::from_str(response_json)
        .unwrap_or_else(|_| json!({ "content": response_json }));

    // Try common response formats
    if let Some(content) = v.get("content").and_then(|c| c.as_str()) {
        return Ok(content.to_string());
    }
    if let Some(text) = v.get("text").and_then(|t| t.as_str()) {
        return Ok(text.to_string());
    }
    // Fallback: treat the entire response as text
    Ok(response_json.to_string())
}

/// Check if the response contains tool calls and extract them.
pub fn extract_tool_calls(response_json: &str) -> Vec<rig_core::completion::message::ToolCall> {
    let v: Value = match serde_json::from_str(response_json) {
        Ok(v) => v,
        Err(_) => return vec![],
    };

    let tool_calls = v.get("tool_calls")
        .and_then(|tc| tc.as_array())
        .cloned()
        .unwrap_or_default();

    tool_calls.iter().filter_map(|tc| {
        let name = tc.get("function")?.get("name")?.as_str()?;
        let args = tc.get("function")?.get("arguments")?;
        Some(rig_core::completion::message::ToolCall::new(
            name.to_string(),
            args.to_string(),
        ))
    }).collect()
}
```

- [ ] **Step 2: Implement CompletionModel for LiteRTCompletionModel**

```rust
// rig-litert/src/completion.rs
use crate::convert;
use crate::error::RigLiteRtError;
use litert_sys::{ConversationConfig, LlmConversation, LlmEngine};
use rig_core::completion::message::{AssistantContent, Text};
use rig_core::completion::{
    CompletionError, CompletionRequest, CompletionResponse, GetTokenUsage, Usage,
};
use rig_core::streaming::StreamingCompletionResponse;
use rig_core::OneOrMany;
use serde::{Deserialize, Serialize};
use std::sync::Arc;
use tokio::sync::Mutex;

/// Raw response type from LiteRT-LM — stored in CompletionResponse<T> as raw_response.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LiteRTResponse {
    pub text: String,
}

/// Streaming response chunk. Placeholder — full streaming in Task 7.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LiteRTStreamingResponse {
    pub text: String,
}

impl GetTokenUsage for LiteRTStreamingResponse {
    fn token_usage(&self) -> Option<Usage> {
        // LiteRT-LM's C API doesn't expose per-stream token counts
        None
    }
}

/// Rig CompletionModel backed by LiteRT-LM's Conversation API.
#[derive(Clone)]
pub struct LiteRTCompletionModel {
    pub(crate) engine: Arc<LlmEngine>,
    pub(crate) model_name: String,
}

impl LiteRTCompletionModel {
    pub fn new(engine: Arc<LlmEngine>, model_name: String) -> Self {
        Self { engine, model_name }
    }
}

impl rig_core::completion::CompletionModel for LiteRTCompletionModel {
    type Response = LiteRTResponse;
    type StreamingResponse = LiteRTStreamingResponse;
    type Client = crate::provider::LiteRTProvider;

    fn make(client: &Self::Client, model: impl Into<String>) -> Self {
        client.completion_model(&model.into())
    }

    async fn completion(
        &self,
        request: CompletionRequest,
    ) -> Result<CompletionResponse<Self::Response>, CompletionError> {
        // 1. Extract system message and tools for conversation setup
        let system_msg = convert::extract_system_message(&request);
        let tools_json = convert::tools_to_json(&request.tools);

        // 2. Create a conversation for this request
        //    Each completion() call creates a fresh conversation to match Rig's stateless model.
        let conv_config = ConversationConfig {
            system_message_json: system_msg,
            tools_json,
            max_output_tokens: request.max_tokens.map(|t| t as i32),
            temperature: request.temperature.map(|t| t as f32),
            top_k: None,
            top_p: None,
        };

        let mut conversation = LlmConversation::new(self.engine.clone(), &conv_config)
            .map_err(|e| CompletionError::ProviderError(e.to_string()))?;

        // 3. Build the user message JSON
        let message_json = convert::build_message_json(&request);
        let extra_context = convert::build_extra_context(&request);

        // 4. Send message (blocking call, wrapped in spawn_blocking for async)
        let response_json = tokio::task::spawn_blocking(move || {
            conversation.send_message(&message_json, extra_context.as_deref())
        })
        .await
        .map_err(|e| CompletionError::ProviderError(format!("task join error: {e}")))?
        .map_err(|e| CompletionError::ProviderError(e.to_string()))?;

        // 5. Parse the response
        let text = convert::parse_response_text(&response_json)
            .map_err(|e| CompletionError::ProviderError(e.to_string()))?;

        let tool_calls = convert::extract_tool_calls(&response_json);

        // 6. Build Rig's CompletionResponse
        let mut choice_items: Vec<AssistantContent> = vec![];
        if !text.is_empty() {
            choice_items.push(AssistantContent::Text(Text { text: text.clone() }));
        }
        for tc in tool_calls {
            choice_items.push(AssistantContent::ToolCall(tc));
        }

        let choice = if choice_items.is_empty() {
            OneOrMany::one(AssistantContent::Text(Text { text: String::new() }))
        } else {
            OneOrMany::many(choice_items)
                .unwrap_or_else(|_| OneOrMany::one(AssistantContent::Text(Text { text: String::new() })))
        };

        Ok(CompletionResponse {
            choice,
            usage: Usage::default(),
            raw_response: LiteRTResponse { text },
            message_id: None,
        })
    }

    async fn stream(
        &self,
        _request: CompletionRequest,
    ) -> Result<StreamingCompletionResponse<Self::StreamingResponse>, CompletionError> {
        // Placeholder — implemented in Task 7
        Err(CompletionError::ProviderError("streaming not yet implemented".into()))
    }
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cargo check -p rig-litert`
Expected: Compiles. Rig type imports may need adjustment based on exact rig-core version — follow compiler errors.

- [ ] **Step 4: Commit**

```bash
git add rig-litert/src/completion.rs rig-litert/src/convert.rs
git commit -m "feat(rig-litert): implement CompletionModel trait (non-streaming)"
```

---

### Task 7: rig-litert — Implement CompletionModel Streaming

**Files:**
- Modify: `rig-litert/src/streaming.rs`
- Modify: `rig-litert/src/completion.rs` (fill in `stream()` method)

**Context:** LiteRT-LM streams via a C callback from a background thread. Rig expects an async `Stream`. We bridge them with a tokio mpsc channel.

- [ ] **Step 1: Write streaming.rs — callback-to-channel bridge**

```rust
// rig-litert/src/streaming.rs
use crate::completion::LiteRTStreamingResponse;
use rig_core::streaming::RawStreamingChoice;
use tokio::sync::mpsc;

/// Bridges LiteRT-LM's C streaming callback to a tokio channel.
///
/// Returns a sender (for the C callback) and a receiver (for the async stream).
pub fn create_stream_channel() -> (
    mpsc::UnboundedSender<StreamChunk>,
    mpsc::UnboundedReceiver<StreamChunk>,
) {
    mpsc::unbounded_channel()
}

/// A chunk from the streaming callback.
pub enum StreamChunk {
    /// A piece of generated text.
    Text(String),
    /// Stream finished.
    Done,
    /// An error occurred.
    Error(String),
}
```

- [ ] **Step 2: Implement stream() in completion.rs**

Replace the placeholder `stream()` method in `LiteRTCompletionModel`:

```rust
    async fn stream(
        &self,
        request: CompletionRequest,
    ) -> Result<StreamingCompletionResponse<Self::StreamingResponse>, CompletionError> {
        use crate::streaming::{StreamChunk, create_stream_channel};
        use async_stream::try_stream;
        use futures::StreamExt;

        let system_msg = convert::extract_system_message(&request);
        let tools_json = convert::tools_to_json(&request.tools);

        let conv_config = ConversationConfig {
            system_message_json: system_msg,
            tools_json,
            max_output_tokens: request.max_tokens.map(|t| t as i32),
            temperature: request.temperature.map(|t| t as f32),
            top_k: None,
            top_p: None,
        };

        let engine = self.engine.clone();
        let message_json = convert::build_message_json(&request);
        let extra_context = convert::build_extra_context(&request);

        let (tx, mut rx) = create_stream_channel();

        // Spawn the C streaming call in a blocking thread
        tokio::task::spawn_blocking(move || {
            let mut conversation = match LlmConversation::new(engine, &conv_config) {
                Ok(c) => c,
                Err(e) => {
                    let _ = tx.send(StreamChunk::Error(e.to_string()));
                    return;
                }
            };

            let tx_clone = tx.clone();
            let result = conversation.send_message_stream(
                &message_json,
                extra_context.as_deref(),
                move |chunk: &str, is_final: bool| {
                    if is_final {
                        let _ = tx_clone.send(StreamChunk::Done);
                    } else if !chunk.is_empty() {
                        let _ = tx_clone.send(StreamChunk::Text(chunk.to_string()));
                    }
                },
            );

            if let Err(e) = result {
                let _ = tx.send(StreamChunk::Error(e.to_string()));
            }
        });

        // Convert the channel into a Rig-compatible stream
        let stream = try_stream! {
            while let Some(chunk) = rx.recv().await {
                match chunk {
                    StreamChunk::Text(text) => {
                        yield RawStreamingChoice::Message(text);
                    }
                    StreamChunk::Done => {
                        yield RawStreamingChoice::FinalResponse(LiteRTStreamingResponse {
                            text: String::new(),
                        });
                        break;
                    }
                    StreamChunk::Error(e) => {
                        Err(CompletionError::ProviderError(e))?;
                    }
                }
            }
        };

        Ok(StreamingCompletionResponse::new(stream))
    }
```

- [ ] **Step 3: Verify it compiles**

Run: `cargo check -p rig-litert`
Expected: Compiles. The `RawStreamingChoice` and `StreamingCompletionResponse::new()` APIs may need adjustment based on exact rig-core version. Follow compiler errors.

- [ ] **Step 4: Commit**

```bash
git add rig-litert/src/streaming.rs rig-litert/src/completion.rs
git commit -m "feat(rig-litert): implement streaming CompletionModel via callback→channel bridge"
```

---

### Task 8: rig-litert — Implement EmbeddingModel

**Files:**
- Modify: `rig-litert/src/embedding.rs`

**Reference:** Rig's `EmbeddingModel` trait at `rig-core/src/embeddings/embedding.rs:48-78`. Key: Rig's `Embedding` uses `Vec<f64>` (not f32), and has a `document: String` field.

- [ ] **Step 1: Implement EmbeddingModel for LiteRTEmbeddingModel**

```rust
// rig-litert/src/embedding.rs
use crate::tokenizer_wrapper::EmbeddingTokenizer;
use litert_sys::TfLiteRunner;
use rig_core::embeddings::{self, EmbeddingError};
use std::sync::Arc;
use tokio::sync::Mutex;

#[derive(Clone)]
pub struct LiteRTEmbeddingModel {
    pub(crate) runner: Arc<Mutex<TfLiteRunner>>,
    pub(crate) tokenizer: Arc<EmbeddingTokenizer>,
    pub(crate) dims: usize,
    pub(crate) model_name: String,
}

impl LiteRTEmbeddingModel {
    pub fn new(
        runner: Arc<Mutex<TfLiteRunner>>,
        tokenizer: Arc<EmbeddingTokenizer>,
        dims: usize,
        model_name: String,
    ) -> Self {
        Self { runner, tokenizer, dims, model_name }
    }
}

/// Mean-pool token embeddings and L2-normalize the result.
///
/// Input: raw model output of shape [seq_len * hidden_dim] (flattened).
/// Output: single vector of length `dims`.
fn mean_pool_and_normalize(raw: &[f32], dims: usize) -> Vec<f64> {
    if raw.is_empty() || dims == 0 {
        return vec![0.0; dims];
    }

    let seq_len = raw.len() / dims;
    if seq_len == 0 {
        return vec![0.0; dims];
    }

    // Mean pooling across the sequence dimension
    let mut pooled = vec![0.0f64; dims];
    for token_idx in 0..seq_len {
        let offset = token_idx * dims;
        for dim_idx in 0..dims {
            pooled[dim_idx] += raw[offset + dim_idx] as f64;
        }
    }
    for v in pooled.iter_mut() {
        *v /= seq_len as f64;
    }

    // L2 normalization
    let norm: f64 = pooled.iter().map(|v| v * v).sum::<f64>().sqrt();
    if norm > 1e-12 {
        for v in pooled.iter_mut() {
            *v /= norm;
        }
    }

    pooled
}

impl embeddings::EmbeddingModel for LiteRTEmbeddingModel {
    const MAX_DOCUMENTS: usize = 64;
    type Client = crate::provider::LiteRTProvider;

    fn make(client: &Self::Client, model: impl Into<String>, dims: Option<usize>) -> Self {
        let model_name = model.into();
        let dims = dims.unwrap_or(client.config().embedding_dims);
        client.embedding_model_with_dims(&model_name, dims)
    }

    fn ndims(&self) -> usize {
        self.dims
    }

    async fn embed_texts(
        &self,
        texts: impl IntoIterator<Item = String>,
    ) -> Result<Vec<embeddings::Embedding>, EmbeddingError> {
        let texts: Vec<String> = texts.into_iter().collect();
        let mut results = Vec::with_capacity(texts.len());

        for text in &texts {
            // 1. Tokenize
            let token_ids = self.tokenizer.encode(text);

            // 2. Run TFLite inference (in blocking context since it's CPU-bound)
            let runner = self.runner.clone();
            let token_ids_clone = token_ids.clone();
            let dims = self.dims;

            let embedding_vec = tokio::task::spawn_blocking(move || {
                // We need to acquire the lock in the blocking context
                let rt = tokio::runtime::Handle::current();
                let mut runner_guard = rt.block_on(runner.lock());
                let raw = runner_guard.invoke(&token_ids_clone)
                    .map_err(|e| EmbeddingError::ProviderError(e.to_string()))?;

                // 3. Mean pool + normalize
                Ok::<Vec<f64>, EmbeddingError>(mean_pool_and_normalize(&raw, dims))
            })
            .await
            .map_err(|e| EmbeddingError::ProviderError(format!("task join error: {e}")))??;

            results.push(embeddings::Embedding {
                document: text.clone(),
                vec: embedding_vec,
            });
        }

        Ok(results)
    }
}
```

- [ ] **Step 2: Add helper method to LiteRTProvider for make() support**

Add this method to `rig-litert/src/provider.rs` inside `impl LiteRTProvider`:

```rust
    /// Create an embedding model with explicit dimensions (used by EmbeddingModel::make).
    pub fn embedding_model_with_dims(&self, model_name: &str, dims: usize) -> LiteRTEmbeddingModel {
        LiteRTEmbeddingModel::new(
            self.runner.clone(),
            self.tokenizer.clone(),
            dims,
            model_name.to_string(),
        )
    }
```

And make `config` field accessible:

```rust
    /// Access the config (needed by EmbeddingModel::make).
    pub fn config(&self) -> &LiteRTConfig {
        &self.config
    }
```

- [ ] **Step 3: Write test for mean_pool_and_normalize**

```rust
// Add at bottom of rig-litert/src/embedding.rs

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_mean_pool_and_normalize_basic() {
        // 2 tokens, 3 dimensions
        // token 0: [1.0, 0.0, 0.0]
        // token 1: [0.0, 1.0, 0.0]
        let raw = vec![1.0f32, 0.0, 0.0, 0.0, 1.0, 0.0];
        let result = mean_pool_and_normalize(&raw, 3);

        // Mean: [0.5, 0.5, 0.0]
        // Norm: sqrt(0.25 + 0.25) = sqrt(0.5) ≈ 0.7071
        // Normalized: [0.7071, 0.7071, 0.0]
        assert_eq!(result.len(), 3);
        assert!((result[0] - 0.7071).abs() < 0.001);
        assert!((result[1] - 0.7071).abs() < 0.001);
        assert!((result[2] - 0.0).abs() < 0.001);
    }

    #[test]
    fn test_mean_pool_and_normalize_empty() {
        let result = mean_pool_and_normalize(&[], 3);
        assert_eq!(result, vec![0.0; 3]);
    }

    #[test]
    fn test_mean_pool_and_normalize_single_token() {
        // 1 token, 2 dimensions: [3.0, 4.0]
        let raw = vec![3.0f32, 4.0];
        let result = mean_pool_and_normalize(&raw, 2);
        // No pooling needed, just normalize
        // Norm: sqrt(9 + 16) = 5
        // Normalized: [0.6, 0.8]
        assert!((result[0] - 0.6).abs() < 0.001);
        assert!((result[1] - 0.8).abs() < 0.001);
    }
}
```

- [ ] **Step 4: Run the test**

Run: `cargo test -p rig-litert -- embedding::tests`
Expected: All 3 tests pass.

- [ ] **Step 5: Commit**

```bash
git add rig-litert/src/embedding.rs rig-litert/src/provider.rs
git commit -m "feat(rig-litert): implement EmbeddingModel trait with mean pooling + L2 normalization"
```

---

### Task 9: edge-agent — Config System + CLI

**Files:**
- Create: `edge-agent/src/config.rs`
- Modify: `edge-agent/src/main.rs`

- [ ] **Step 1: Write config.rs**

```rust
// edge-agent/src/config.rs
use serde::Deserialize;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Deserialize)]
pub struct Config {
    pub llm: LlmConfig,
    pub embedding: EmbeddingConfig,
    pub agent: AgentConfig,
}

#[derive(Debug, Clone, Deserialize)]
pub struct LlmConfig {
    pub provider: ProviderKind,
    pub local: Option<LocalLlmConfig>,
    pub openai: Option<OpenAIConfig>,
    pub ollama: Option<OllamaConfig>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct EmbeddingConfig {
    pub provider: ProviderKind,
    pub local: Option<LocalEmbeddingConfig>,
    pub openai: Option<OpenAIEmbeddingConfig>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum ProviderKind {
    Local,
    OpenAI,
    Ollama,
}

#[derive(Debug, Clone, Deserialize)]
pub struct LocalLlmConfig {
    pub model_path: PathBuf,
    pub backend: String,       // "cpu" | "gpu" | "npu"
    pub num_threads: Option<usize>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct OpenAIConfig {
    pub base_url: Option<String>,
    pub api_key: Option<String>, // supports ${ENV_VAR} syntax
    pub model: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct OllamaConfig {
    pub base_url: String,
    pub model: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct LocalEmbeddingConfig {
    pub model_path: PathBuf,
    pub tokenizer_path: PathBuf,
    pub dims: usize,
}

#[derive(Debug, Clone, Deserialize)]
pub struct OpenAIEmbeddingConfig {
    pub model: String,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AgentConfig {
    pub system_prompt: String,
    pub temperature: Option<f64>,
    pub max_tokens: Option<u64>,
}

impl Config {
    /// Load config from a YAML file.
    pub fn from_file(path: &Path) -> anyhow::Result<Self> {
        let content = std::fs::read_to_string(path)?;
        // Expand environment variables like ${OPENAI_API_KEY}
        let expanded = shellexpand::env(&content)
            .map_err(|e| anyhow::anyhow!("env var expansion failed: {e}"))?;
        let config: Config = serde_yaml::from_str(&expanded)?;
        Ok(config)
    }
}
```

- [ ] **Step 2: Add shellexpand dependency to Cargo.toml**

Add to `edge-agent/Cargo.toml` under `[dependencies]`:

```toml
shellexpand = "3"
```

- [ ] **Step 3: Write main.rs with CLI and REPL**

```rust
// edge-agent/src/main.rs
mod config;

use anyhow::Result;
use clap::Parser;
use config::{Config, ProviderKind};
use std::io::{self, BufRead, Write};
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "edge-agent", about = "On-device AI agent powered by LiteRT-LM + Rig")]
struct Cli {
    /// Path to config file
    #[arg(short, long, default_value = "edge-agent.yaml")]
    config: PathBuf,

    /// Override LLM provider
    #[arg(long)]
    llm_provider: Option<ProviderKind>,

    /// Override embedding provider
    #[arg(long)]
    embedding_provider: Option<ProviderKind>,

    /// Single query mode (non-interactive)
    #[arg(long)]
    once: Option<String>,
}

// Rig's CompletionModel has associated types, so we can't use Box<dyn ...>.
// Use an enum to dispatch at runtime.
enum AnyAgent {
    LiteRT(rig_core::agent::Agent<rig_litert::LiteRTCompletionModel>),
    // OpenAI and Ollama variants will be added when those integrations are wired up.
    // For now, only local is functional.
}

impl AnyAgent {
    async fn prompt(&self, input: &str) -> Result<String> {
        use rig_core::completion::Prompt;
        match self {
            Self::LiteRT(a) => Ok(a.prompt(input).await?),
        }
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    let mut config = Config::from_file(&cli.config)?;

    // CLI overrides
    if let Some(provider) = cli.llm_provider {
        config.llm.provider = provider;
    }
    if let Some(provider) = cli.embedding_provider {
        config.embedding.provider = provider;
    }

    // Build agent based on config
    let agent = match config.llm.provider {
        ProviderKind::Local => {
            let local = config.llm.local
                .as_ref()
                .ok_or_else(|| anyhow::anyhow!("llm.local config required when provider is 'local'"))?;

            let backend = match local.backend.as_str() {
                "gpu" => litert_sys::Backend::Gpu,
                "npu" => litert_sys::Backend::Npu,
                _ => litert_sys::Backend::Cpu,
            };

            let emb_local = config.embedding.local
                .as_ref()
                .ok_or_else(|| anyhow::anyhow!("embedding.local config required"))?;

            let provider = rig_litert::LiteRTProvider::new(rig_litert::LiteRTConfig {
                llm_model_path: local.model_path.clone(),
                embedding_model_path: emb_local.model_path.clone(),
                embedding_tokenizer_path: emb_local.tokenizer_path.clone(),
                backend,
                embedding_num_threads: local.num_threads.unwrap_or(4),
                embedding_dims: emb_local.dims,
                max_tokens: config.agent.max_tokens.map(|t| t as i32),
            })?;

            use rig_core::completion::CompletionModel;
            let model = provider.completion_model("local");
            let mut builder = model.agent(&config.agent.system_prompt);
            if let Some(temp) = config.agent.temperature {
                builder = builder.temperature(temp);
            }
            if let Some(max) = config.agent.max_tokens {
                builder = builder.max_tokens(max);
            }
            AnyAgent::LiteRT(builder.build())
        }
        ProviderKind::OpenAI => {
            anyhow::bail!("OpenAI provider not yet wired up — use 'local' or contribute the integration!")
        }
        ProviderKind::Ollama => {
            anyhow::bail!("Ollama provider not yet wired up — use 'local' or contribute the integration!")
        }
    };

    // Single query mode
    if let Some(query) = cli.once {
        let response = agent.prompt(&query).await?;
        println!("{response}");
        return Ok(());
    }

    // Interactive REPL
    println!("Edge Agent ready. Type 'quit' to exit.");
    let stdin = io::stdin();
    loop {
        print!(">>> ");
        io::stdout().flush()?;

        let mut line = String::new();
        if stdin.lock().read_line(&mut line)? == 0 {
            break; // EOF
        }
        let line = line.trim();
        if line == "quit" || line == "exit" {
            break;
        }
        if line.is_empty() {
            continue;
        }

        match agent.prompt(line).await {
            Ok(response) => println!("{response}"),
            Err(e) => eprintln!("Error: {e}"),
        }
    }

    Ok(())
}
```

- [ ] **Step 4: Create example config file**

```yaml
# edge-agent.yaml

llm:
  provider: local
  local:
    model_path: models/gemma-2b.litertlm
    backend: cpu
    num_threads: 4
  openai:
    api_key: ${OPENAI_API_KEY}
    model: gpt-4o
  ollama:
    base_url: http://localhost:11434
    model: llama3

embedding:
  provider: local
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

- [ ] **Step 5: Verify it compiles**

Run: `cargo check -p edge-agent`
Expected: Compiles.

- [ ] **Step 6: Write a test for config parsing**

```rust
// Add at bottom of edge-agent/src/config.rs

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn test_parse_config() {
        let yaml = r#"
llm:
  provider: local
  local:
    model_path: models/test.litertlm
    backend: cpu

embedding:
  provider: local
  local:
    model_path: models/test.tflite
    tokenizer_path: models/tokenizer.json
    dims: 384

agent:
  system_prompt: "test prompt"
  temperature: 0.5
  max_tokens: 1024
"#;
        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        write!(tmpfile, "{yaml}").unwrap();

        let config = Config::from_file(tmpfile.path()).unwrap();
        assert_eq!(config.llm.provider, ProviderKind::Local);
        assert_eq!(config.embedding.local.unwrap().dims, 384);
        assert_eq!(config.agent.system_prompt, "test prompt");
    }
}
```

Add `tempfile = "3"` to `edge-agent/Cargo.toml` under `[dev-dependencies]`.

- [ ] **Step 7: Run the test**

Run: `cargo test -p edge-agent -- config::tests`
Expected: PASS.

- [ ] **Step 8: Commit**

```bash
git add edge-agent/ edge-agent.yaml
git commit -m "feat(edge-agent): add config system, CLI, and REPL"
```

---

### Task 10: Integration Verification

**Files:** No new files — this task verifies end-to-end compilation and documents known gaps.

- [ ] **Step 1: Full workspace build check**

Run: `cargo check --workspace`
Expected: All three crates compile. Note any warnings.

- [ ] **Step 2: Run all tests**

Run: `cargo test --workspace`
Expected: `mean_pool_and_normalize` tests and config parsing test pass.

- [ ] **Step 3: Document known gaps for future work**

Create a tracking checklist as a comment in the workspace Cargo.toml or as a separate file:

```markdown
<!-- Known gaps — to be resolved during integration testing with real models -->

1. [ ] litert-sys/build.rs: CMake flags need validation against actual LiteRT-LM build system
2. [ ] litert-sys/src/tflite.rs: TfLiteRunner implementation is a placeholder — needs TFLite C API wiring
3. [ ] rig-litert/src/convert.rs: JSON format for LiteRT-LM conversation messages needs validation with real model output
4. [ ] rig-litert/src/completion.rs: Tool call parsing from LiteRT-LM response needs testing with tool-capable models
5. [ ] rig-litert/src/streaming.rs: Callback lifetime management needs testing under real streaming conditions
6. [ ] edge-agent: OpenAI and Ollama provider variants in AnyAgent not yet wired up
7. [ ] Cross-compilation: Not yet tested on Android NDK or ARM Linux toolchains
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: full workspace build verification, document known integration gaps"
```

---

## Summary

| Task | Crate | What it delivers |
|------|-------|-----------------|
| 1 | workspace | Scaffold, git submodule, 3 crate skeletons |
| 2 | litert-sys | build.rs + bindgen for LiteRT-LM C API |
| 3 | litert-sys | LlmEngine + LlmConversation safe wrappers |
| 4 | litert-sys | TfLiteRunner interface for embedding models |
| 5 | rig-litert | Provider struct, error conversions, module stubs |
| 6 | rig-litert | CompletionModel (non-streaming) |
| 7 | rig-litert | CompletionModel streaming via callback→channel |
| 8 | rig-litert | EmbeddingModel with mean pooling + L2 norm |
| 9 | edge-agent | Config YAML, CLI, REPL, provider selection |
| 10 | all | Full build verification + gap documentation |

After Task 10, the project compiles and has the complete architecture in place. The remaining work (resolving the gaps in Task 10's checklist) requires real models and devices for integration testing.
