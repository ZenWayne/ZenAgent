# P9 — JNI + Kotlin DSL (JVM MVP)

**Branch:** `feat/p9-jni-kotlin-mvp`
**Spec anchor:** §7 (Kotlin DSL), §8 (JNI), §15 acceptance demos A & B (Android).
**Goal (MVP):** Prove the JNI ↔ Kotlin DSL ↔ C++ Runner edge works end-to-end on plain JVM with a real LiteRT-LM model. **No Android in this phase** — Android is build-system work on top of a working MVP, and isolating it lets us verify the JNI shape first.

## What's in (MVP)

- `jni/agentflow_jni.cc` — single C function exported through JNI:
  `runAgent(modelPath, systemPrompt, userQuery, toolName?, toolJsonResult?, constrainedToolCalls)`
  → returns the assistant's final reply as a UTF-8 string.
  Internally creates a `LiteRtLmEngine`, builds a one-agent graph, runs through `Runner`, returns `assistant_reply`.
- `libagentflow_jni.so` produced by Bazel (`cc_binary` with `linkshared = True`).
- `kotlin/` Gradle project (Kotlin/JVM only):
  - `agentflow.jni.NativeBridge` — `external fun runAgent(...)`, plus `System.loadLibrary("agentflow_jni")`.
  - `agentflow.dsl.Workflow` — `workflow { state(...); agent("name") { ... } }.run(input)` builder.
  - `agentflow.runtime.AgentRunResult` — return type carrying assistant text.
- One JVM smoke test (`SmokeTest.kt`) gated on `MODEL_PATH`: builds + runs a single-agent workflow, asserts the reply is non-empty.
- A `kotlin/scripts/build_native.sh` helper: runs Bazel, copies `libagentflow_jni.so` into `kotlin/build/libs/native/`.

## What's out (deferred to later phases)

- Multi-node DSL (`team`, `router`, `aggregator`) — needs richer graph-spec marshalling.
- Tool registration via Kotlin lambdas — needs JNI callback bridge (JavaVM cache + `AttachCurrentThread`).
- Streaming `Flow<TraceEvent>` — needs JNI callback to push tokens to `callbackFlow`.
- Cancellation token via `awaitClose` — same JNI callback machinery.
- Protobuf `GraphSpec` marshalling — MVP passes plain strings.
- Android target — needs AGP + NDK arm64-v8a build + emulator.
- Kotlin Multiplatform — JVM only for MVP.

These are tracked as P10+ items. Each one is independently shippable on top of P9's plumbing.

## File Structure

```
jni/                                  # NEW
  BUILD.bazel                          # cc_binary linkshared=True → libagentflow_jni.so
  agentflow_jni.cc                     # JNI entry points
kotlin/                                # NEW
  build.gradle.kts                     # Kotlin/JVM project, JUnit5
  settings.gradle.kts
  src/main/kotlin/agentflow/
    jni/NativeBridge.kt                # external fun runAgent(...)
    dsl/Workflow.kt                    # workflow { ... } builder
    dsl/Agent.kt                       # agent("name") { ... }
    runtime/AgentRunResult.kt          # return shape
  src/test/kotlin/agentflow/
    SmokeTest.kt                       # MODEL_PATH-gated JUnit5 test
  scripts/
    build_native.sh                    # bazel build + copy .so
  .gitignore                           # ignore build/, .gradle/
docs/superpowers/plans/
  2026-06-05-cpp-agent-framework-p9-jni-kotlin-mvp.md   # this file
```

## Task Dependency

```
T1 (plan) → T2 (JNI C++) → T3 (Kotlin DSL + NativeBridge) → T4 (JVM test) → T5 (PR)
```

## Task 2 — JNI C++ side

**Files:** `jni/agentflow_jni.cc`, `jni/BUILD.bazel`

### Step 2.1: `agentflow_jni.cc`

One JNI entry point:

```cpp
extern "C" JNIEXPORT jstring JNICALL
Java_agentflow_jni_NativeBridge_runAgent(
    JNIEnv* env, jobject /*self*/,
    jstring model_path_j,
    jstring system_prompt_j,
    jstring user_query_j,
    jboolean constrained_tool_calls);
```

Implementation:

1. Convert jstrings to std::string via `GetStringUTFChars` / `ReleaseStringUTFChars`.
2. `LiteRtLmEngine::Create({.model_path = ...})` — null check.
3. Build one-node graph: `AgentNode` with `system_prompt`, `input_field="user_query"`, `output_field="assistant_reply"`, optional `constrained_tool_calls`.
4. Bootstrap a `TestState` proto with `set_user_query(user_query)`. Wrap as `State`.
5. Run via `Runner` on a local `asio::io_context` (sync; we block the JNI thread until done).
6. Extract `assistant_reply` field.
7. Return as `jstring` via `NewStringUTF`.

On any exception, return `nullptr` and `env->ThrowNew(jclass, msg)`.

MVP intentionally skips: tool registration, streaming, cancel. They land in P10.

### Step 2.2: BUILD.bazel

```python
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "agentflow_jni",
    srcs = ["agentflow_jni.cc"],
    linkshared = True,           # → libagentflow_jni.so
    deps = [
        "//agentflow/core",
        "//agentflow/nodes",
        "//agentflow/tools",
        "//agentflow/inference",
        "//proto:agentflow_proto",
        "@asio",
    ],
)
```

JDK headers: rely on system `JAVA_HOME` headers; Bazel picks up `<jni.h>` from `$JAVA_HOME/include` automatically via `@bazel_tools//tools/jdk:jni`. If that linkage breaks, fall back to hard-coded `linkopts = ["-I$(JAVA_HOME)/include", "-I$(JAVA_HOME)/include/linux"]` in `copts`.

Commit: `feat(p9): JNI bridge — libagentflow_jni.so exports runAgent`

## Task 3 — Kotlin DSL + NativeBridge

**Files:** under `kotlin/`

### Step 3.1: `build.gradle.kts`

Plain Kotlin/JVM project:

```kotlin
plugins {
    kotlin("jvm") version "2.1.0"
    application
}
repositories { mavenCentral() }
dependencies {
    testImplementation("org.junit.jupiter:junit-jupiter:5.10.2")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}
tasks.test { useJUnitPlatform() }

// java.library.path so System.loadLibrary("agentflow_jni") works
tasks.withType<Test> {
    systemProperty("java.library.path", "build/libs/native:${System.getProperty("java.library.path")}")
}
```

### Step 3.2: `NativeBridge.kt`

```kotlin
package agentflow.jni

internal object NativeBridge {
    init {
        System.loadLibrary("agentflow_jni")
    }

    external fun runAgent(
        modelPath: String,
        systemPrompt: String,
        userQuery: String,
        constrainedToolCalls: Boolean,
    ): String
}
```

### Step 3.3: DSL

```kotlin
package agentflow.dsl

class AgentBuilder(val id: String) {
    var modelPath: String = ""
    var systemPrompt: String = "You are a helpful assistant."
    var constrainedToolCalls: Boolean = false
}

class WorkflowBuilder {
    internal var agent: AgentBuilder? = null
    fun agent(id: String, block: AgentBuilder.() -> Unit): AgentBuilder {
        val b = AgentBuilder(id).apply(block)
        agent = b
        return b
    }
}

class Workflow internal constructor(private val agent: AgentBuilder) {
    fun run(userQuery: String): String {
        require(agent.modelPath.isNotEmpty()) { "agent.modelPath must be set" }
        return NativeBridge.runAgent(
            agent.modelPath,
            agent.systemPrompt,
            userQuery,
            agent.constrainedToolCalls,
        )
    }
}

fun workflow(block: WorkflowBuilder.() -> Unit): Workflow {
    val b = WorkflowBuilder().apply(block)
    val agent = requireNotNull(b.agent) { "workflow must declare at least one agent {...}" }
    return Workflow(agent)
}
```

The MVP DSL is intentionally tiny — single agent, no edges. This is enough to demonstrate the pattern; richer DSL is a P10 feature.

Commit: `feat(p9): Kotlin DSL workflow { agent {} } + NativeBridge`

## Task 4 — JVM smoke test

**File:** `kotlin/src/test/kotlin/agentflow/SmokeTest.kt`

```kotlin
package agentflow

import agentflow.dsl.workflow
import org.junit.jupiter.api.Assertions.assertFalse
import org.junit.jupiter.api.Assumptions.assumeTrue
import org.junit.jupiter.api.Test

class SmokeTest {
    @Test
    fun realModelAnswersHello() {
        val modelPath = System.getenv("MODEL_PATH")
        assumeTrue(modelPath != null) { "MODEL_PATH not set — skipping" }

        val flow = workflow {
            agent("chat") {
                this.modelPath = modelPath!!
                systemPrompt = "Reply in one short sentence."
            }
        }
        val reply = flow.run("Say hello.")
        println("Reply: $reply")
        assertFalse(reply.isBlank(), "expected non-empty reply")
    }
}
```

Commit: `test(p9): JVM smoke test gated on MODEL_PATH`

## Task 5 — Tag + PR

- Tag `p9-jni-kotlin-mvp`.
- Push branch, open PR against `master`.
- PR body explains: this is the JVM MVP closing the JNI edge; Android + streaming Flow + tool callbacks are tracked as P10+.

## Verification

```bash
# Build the native .so
bash kotlin/scripts/build_native.sh

# Run hermetic Kotlin tests (no model)
cd kotlin && ./gradlew test

# Run smoke test with real model
MODEL_PATH=$(realpath ../models/gemma-4-E2B-it.litertlm) ./gradlew test
```

## Risks / known-unknowns

- **JNI linkage to LiteRT-LM archives.** `libagentflow_jni.so` will pull in the heavy `libce_*.a` static archives. The link is identical to `agent-demo` so the same `--allow-multiple-definition` linker flag applies. Should work — same pattern, but slot for surprise.
- **JVM crashes on UTF-8 / NUL bytes.** `GetStringUTFChars` returns modified UTF-8 (NUL becomes 0xC0 0x80). For model output that's typically fine but worth noting in docs.
- **Bazel `linkshared = True` and asio's coroutine PIC code.** Some asio targets don't build cleanly with `-fPIC` in older toolchains. Mitigation: add `copts = ["-fPIC"]` and `features = ["-fully_static_link"]` if needed.

## Out-of-scope (explicit, tracked in plan-of-plans)

- Streaming `Flow<TraceEvent>` (P10)
- Tool registration via Kotlin lambdas (P10)
- Async cancel via `awaitClose` (P10)
- Android target + AGP + NDK arm64 build (P11)
- Demo A / Demo B (P11 / P12 — needs all of P10 first)

## Execution Handoff

Each task ends in a commit. T4's smoke test is the v1-MVP acceptance proof for "Kotlin can drive a real agent through C++."
