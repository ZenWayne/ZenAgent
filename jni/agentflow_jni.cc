// jni/agentflow_jni.cc
//
// JNI entry points for the JVM MVP (P9). The MVP exposes ONE function —
// runAgent — that wraps the same flow as examples/agent-demo: create a
// LiteRtLmEngine, build a single-node graph with an AgentNode, run it on a
// local asio::io_context, return the assistant_reply field as a UTF-8
// jstring.
//
// Multi-node DSL, tool callbacks, streaming Flow, and cancel are P10+.

#include <jni.h>

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>

#include <thread>

#include "agentflow/inference/litert_lm_conversation.h"

#include "agentflow/core/cancel.h"
#include "agentflow/core/errors.h"
#include "agentflow/core/event.h"
#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/core/token_sink.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"
#include "agentflow/workflow/workflow_loader.h"
#include "agentflow/workflow/workflow_runner.h"
#include "test_messages.pb.h"

namespace af = agentflow;
using namespace std::chrono_literals;

namespace {

// Small RAII helper for jstring → std::string with proper release.
class JString {
 public:
  JString(JNIEnv* env, jstring js) : env_(env), js_(js), data_(nullptr) {
    if (js_ != nullptr) {
      data_ = env_->GetStringUTFChars(js_, nullptr);
    }
  }
  ~JString() {
    if (data_ != nullptr) env_->ReleaseStringUTFChars(js_, data_);
  }
  JString(const JString&) = delete;
  JString& operator=(const JString&) = delete;

  std::string str() const {
    return data_ != nullptr ? std::string(data_) : std::string{};
  }

 private:
  JNIEnv* env_;
  jstring js_;
  const char* data_;
};

void ThrowJava(JNIEnv* env, const char* msg) {
  jclass cls = env->FindClass("java/lang/RuntimeException");
  if (cls != nullptr) env->ThrowNew(cls, msg);
}

// Cancellation registry. A streaming run is a blocking JNI call that owns its
// thread, so cancellation must come from a DIFFERENT thread (the Flow's
// awaitClose). We hand Kotlin an opaque id; the run takes the source's Token
// (which holds the shared state independently of the source), and a separate
// nativeCancel(id) flips it. The global mutex serializes cancel vs free so a
// concurrent free can't dangle a cancel.
std::mutex g_cancel_mu;
std::unordered_map<jlong, std::unique_ptr<::agentflow::CancelSource>> g_cancels;
jlong g_next_cancel_id = 1;

// Cached JavaVM, set in JNI_OnLoad. A persistent Session runs the engine on its
// own worker thread (NOT a JVM thread), so token callbacks must attach to the
// VM before calling back into Kotlin.
JavaVM* g_vm = nullptr;

// ── Persistent multi-turn session ────────────────────────────────────────────
//
// A Session owns the heavy, reusable inference state: ONE LiteRtLmEngine (the
// model is loaded once — recreating it per message both reloads ~GBs and trips
// the engine factory's per-process "ALREADY_EXISTS" registration), a persistent
// io_context driven by a dedicated worker thread, the parsed workflow + its
// host tools + keepalive (sub-agent runtime / delegate tools), and a persistent
// conversation slot. Each SendMessage reuses all of it and continues the same
// dialogue (the engine owns history server-side), so multi-turn just works.
struct Session {
  std::shared_ptr<af::LiteRtLmEngine> engine;
  std::shared_ptr<af::workflow::Workflow> workflow;
  std::shared_ptr<af::ToolRegistry> host_tools;
  std::vector<std::shared_ptr<void>> keepalive;
  // Persistent conversation reused across messages (created lazily on the first
  // SendMessage, then continued). Lives on `io`.
  std::shared_ptr<af::LiteRtLmConversation> conversation;

  asio::io_context io;
  // Keeps io.run() alive between messages (no work would otherwise return).
  asio::executor_work_guard<asio::io_context::executor_type> work_guard;
  std::thread worker;
  // Serializes SendMessage calls — one decode at a time per session.
  std::mutex send_mu;

  Session() : work_guard(asio::make_work_guard(io)) {}
};

std::mutex g_session_mu;
std::unordered_map<jlong, std::shared_ptr<Session>> g_sessions;
jlong g_next_session_id = 1;

std::shared_ptr<Session> LookupSession(jlong id) {
  std::lock_guard<std::mutex> lk(g_session_mu);
  auto it = g_sessions.find(id);
  return it == g_sessions.end() ? nullptr : it->second;
}

}  // namespace

extern "C" {

// Java signature:
//   external fun runAgent(
//       modelPath: String,
//       systemPrompt: String,
//       userQuery: String,
//       constrainedToolCalls: Boolean,
//   ): String
JNIEXPORT jstring JNICALL
Java_agentflow_jni_NativeBridge_runAgent(
    JNIEnv* env, jobject /*self*/,
    jstring model_path_j,
    jstring system_prompt_j,
    jstring user_query_j,
    jboolean constrained_tool_calls_j) {
  try {
    const std::string model_path = JString(env, model_path_j).str();
    const std::string system_prompt = JString(env, system_prompt_j).str();
    const std::string user_query = JString(env, user_query_j).str();
    const bool constrained = constrained_tool_calls_j == JNI_TRUE;

    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path});
    if (!engine) {
      ThrowJava(env, "LiteRtLmEngine::Create failed");
      return nullptr;
    }

    asio::io_context io;
    af::AgentNodeConfig agent_cfg;
    agent_cfg.engine = engine;
    agent_cfg.io_ctx = &io;
    agent_cfg.system_prompt = system_prompt;
    agent_cfg.input_field = "user_query";
    agent_cfg.output_field = "assistant_reply";
    agent_cfg.max_iter = 5;
    agent_cfg.stream_tokens = false;
    agent_cfg.constrained_tool_calls = constrained;

    af::GraphBuilder b;
    b.AddNode(std::make_unique<af::AgentNode>(std::move(agent_cfg)))
     .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
     .AddEdge("agent", "sink");
    auto graph = b.Build();

    af::test::TestState init;
    init.set_user_query(user_query);

    af::Runner runner(std::move(graph), af::Runner::Options{});
    auto fut = asio::co_spawn(io,
        [&]() -> asio::awaitable<af::State> {
          co_return co_await runner.Run(af::State::From(init));
        },
        asio::use_future);
    io.run();
    auto out = fut.get();

    const std::string& reply = out.As<af::test::TestState>().assistant_reply();
    return env->NewStringUTF(reply.c_str());
  } catch (const std::exception& e) {
    ThrowJava(env, e.what());
    return nullptr;
  } catch (...) {
    ThrowJava(env, "unknown C++ exception");
    return nullptr;
  }
}

// Java signature:
//   external fun runJsonWorkflow(
//       modelPath: String,
//       workflowJson: String,
//       userQuery: String,
//   ): String
//
// MVP routes the workflow's main agent through the same single-AgentNode
// pipeline as runAgent. Multi-agent + sub-agent + streaming are P16+.
JNIEXPORT jstring JNICALL
Java_agentflow_jni_NativeBridge_runJsonWorkflow(
    JNIEnv* env, jobject /*self*/,
    jstring model_path_j,
    jstring workflow_json_j,
    jstring user_query_j) {
  try {
    const std::string model_path    = JString(env, model_path_j).str();
    const std::string workflow_json = JString(env, workflow_json_j).str();
    const std::string user_query    = JString(env, user_query_j).str();

    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path});
    if (!engine) {
      ThrowJava(env, "LiteRtLmEngine::Create failed");
      return nullptr;
    }

    asio::io_context io;
    // ToolRegistry must be a shared_ptr for the workflow runner.
    auto host_tools = std::make_shared<af::ToolRegistry>(io);

    auto wf_or =
        af::workflow::WorkflowLoader::Load(workflow_json, *host_tools);
    if (!wf_or.ok()) {
      ThrowJava(env, std::string(wf_or.status().message()).c_str());
      return nullptr;
    }
    auto wf = *wf_or;

    af::workflow::AgentNodeBuildSpec build_spec;
    build_spec.workflow     = wf;
    build_spec.agent_name   = wf->spec().main();
    build_spec.host_tools   = host_tools;
    build_spec.engine       = engine;
    build_spec.io_ctx       = &io;
    build_spec.input_field  = "user_query";
    build_spec.output_field = "assistant_reply";
    build_spec.max_iter     = 5;
    auto built = af::workflow::BuildAgentNode(build_spec);
    if (built.cfg.system_prompt.empty() && !built.cfg.engine) {
      ThrowJava(env, "main agent not in roster");
      return nullptr;
    }
    // SubAgentRuntime + delegate tool must outlive the Runner.
    std::vector<std::shared_ptr<void>> keepalive =
        std::move(built.keepalive);

    af::GraphBuilder b;
    b.AddNode(std::make_unique<af::AgentNode>(std::move(built.cfg)))
     .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
     .AddEdge("agent", "sink");
    auto graph = b.Build();

    af::test::TestState init;
    init.set_user_query(user_query);
    af::Runner runner(std::move(graph), af::Runner::Options{});
    auto fut = asio::co_spawn(io,
        [&]() -> asio::awaitable<af::State> {
          co_return co_await runner.Run(af::State::From(init));
        },
        asio::use_future);
    io.run();
    auto out = fut.get();
    const std::string& reply = out.As<af::test::TestState>().assistant_reply();
    return env->NewStringUTF(reply.c_str());
  } catch (const std::exception& e) {
    ThrowJava(env, e.what());
    return nullptr;
  } catch (...) {
    ThrowJava(env, "unknown C++ exception");
    return nullptr;
  }
}

// Java signature:
//   external fun runJsonWorkflowStreaming(
//       modelPath: String,
//       workflowJson: String,
//       userQuery: String,
//       onToken: TokenCallback,   // fun interface { fun onToken(token: String) }
//   ): String
//
// Same workflow pipeline as runJsonWorkflow, but the main agent runs in
// streaming mode: each generated text delta is handed to a TokenSink that
// forwards it to `onToken` as it arrives (a direct path, not the TraceEvent
// stream). The full assistant reply is returned when the run completes.
//
// Streaming requires the unconstrained path (the constrained C bridge has no
// streaming variant), so this entry forces constrained_tool_calls = false.
JNIEXPORT jstring JNICALL
Java_agentflow_jni_NativeBridge_runJsonWorkflowStreaming(
    JNIEnv* env, jobject /*self*/,
    jstring model_path_j,
    jstring workflow_json_j,
    jstring user_query_j,
    jobject on_token_j,
    jlong cancel_id_j) {
  try {
    const std::string model_path    = JString(env, model_path_j).str();
    const std::string workflow_json = JString(env, workflow_json_j).str();
    const std::string user_query    = JString(env, user_query_j).str();

    // Take the cancel token for this run (0 = no cancellation). The token
    // holds the shared state independently of the source, so it stays valid
    // even if Kotlin frees the source after the run.
    ::agentflow::CancelToken cancel_tok;
    if (cancel_id_j != 0) {
      std::lock_guard<std::mutex> lk(g_cancel_mu);
      auto it = g_cancels.find(cancel_id_j);
      if (it != g_cancels.end()) cancel_tok = it->second->Token();
    }

    // Resolve the callback's onToken(String):void method up front.
    jmethodID on_token_mid = nullptr;
    if (on_token_j != nullptr) {
      jclass cb_cls = env->GetObjectClass(on_token_j);
      on_token_mid =
          env->GetMethodID(cb_cls, "onToken", "(Ljava/lang/String;)V");
      if (on_token_mid == nullptr) {
        ThrowJava(env, "callback missing onToken(String) method");
        return nullptr;
      }
    }

    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path});
    if (!engine) {
      ThrowJava(env, "LiteRtLmEngine::Create failed");
      return nullptr;
    }

    asio::io_context io;
    auto host_tools = std::make_shared<af::ToolRegistry>(io);

    auto wf_or =
        af::workflow::WorkflowLoader::Load(workflow_json, *host_tools);
    if (!wf_or.ok()) {
      ThrowJava(env, std::string(wf_or.status().message()).c_str());
      return nullptr;
    }
    auto wf = *wf_or;

    // Run-wide token sink: every generated delta (main agent or any delegated
    // sub-agent) is marshalled onto the JVM callback inline. It runs on this io
    // thread, so `env` is valid for CallVoidMethod (no AttachCurrentThread).
    bool cb_failed = false;
    auto on_delta = [&](std::string_view tok) {
      if (on_token_mid == nullptr || cb_failed) return;
      jstring js = env->NewStringUTF(std::string(tok).c_str());
      env->CallVoidMethod(on_token_j, on_token_mid, js);
      env->DeleteLocalRef(js);
      if (env->ExceptionCheck()) {
        // A Kotlin callback threw — leave the exception pending and stop
        // forwarding (this lambda becomes a no-op for the rest of the run); it
        // surfaces to the caller when JNI returns. A no-op sink never blocks
        // the run, so there is no producer stall the old full-channel path had.
        cb_failed = true;
      }
    };

    af::workflow::AgentNodeBuildSpec build_spec;
    build_spec.workflow      = wf;
    build_spec.agent_name    = wf->spec().main();
    build_spec.host_tools    = host_tools;
    build_spec.engine        = engine;
    build_spec.io_ctx        = &io;
    build_spec.input_field   = "user_query";
    build_spec.output_field  = "assistant_reply";
    build_spec.max_iter      = 5;
    build_spec.token_sink    = on_delta;
    auto built = af::workflow::BuildAgentNode(build_spec);
    if (built.cfg.system_prompt.empty() && !built.cfg.engine) {
      ThrowJava(env, "main agent not in roster");
      return nullptr;
    }
    std::vector<std::shared_ptr<void>> keepalive = std::move(built.keepalive);

    // Streaming requires the unconstrained path (no streaming constrained C
    // bridge). BuildAgentNode already set stream_tokens + on_delta.
    built.cfg.constrained_tool_calls = false;

    af::GraphBuilder b;
    b.AddNode(std::make_unique<af::AgentNode>(std::move(built.cfg)))
     .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
     .AddEdge("agent", "sink");
    auto graph = b.Build();

    af::test::TestState init;
    init.set_user_query(user_query);
    af::Runner runner(std::move(graph), af::Runner::Options{});

    std::string reply;
    std::exception_ptr run_exc;

    // One coroutine: the graph runs and each delta is delivered inline via
    // on_delta during the run — there is no separate consumer. io.run() returns
    // once the run completes.
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
      try {
        auto out = co_await runner.Run(af::State::From(init), cancel_tok);
        reply = out.As<af::test::TestState>().assistant_reply();
      } catch (...) {
        run_exc = std::current_exception();
      }
      co_return;
    }, asio::detached);

    io.run();

    if (run_exc) std::rethrow_exception(run_exc);
    if (env->ExceptionCheck()) return nullptr;  // callback threw
    return env->NewStringUTF(reply.c_str());
  } catch (const std::exception& e) {
    ThrowJava(env, e.what());
    return nullptr;
  } catch (...) {
    ThrowJava(env, "unknown C++ exception");
    return nullptr;
  }
}

// ── Cancellation handle (see g_cancels) ──────────────────────────────────────

// Creates a cancel source and returns its opaque id (pass to
// runJsonWorkflowStreaming, then to nativeCancel/nativeFreeCancel).
JNIEXPORT jlong JNICALL
Java_agentflow_jni_NativeBridge_nativeNewCancel(JNIEnv* /*env*/,
                                                jobject /*self*/) {
  std::lock_guard<std::mutex> lk(g_cancel_mu);
  jlong id = g_next_cancel_id++;
  g_cancels[id] = std::make_unique<::agentflow::CancelSource>();
  return id;
}

// Signals cancellation for a running streaming call. Safe to call from any
// thread; no-op if the id was already freed.
JNIEXPORT void JNICALL
Java_agentflow_jni_NativeBridge_nativeCancel(JNIEnv* /*env*/, jobject /*self*/,
                                             jlong cancel_id_j) {
  std::lock_guard<std::mutex> lk(g_cancel_mu);
  auto it = g_cancels.find(cancel_id_j);
  if (it != g_cancels.end()) it->second->Cancel();
}

// Releases a cancel source. The run's already-taken CancelToken stays valid
// (it holds the shared state), so this is safe once the run has finished.
JNIEXPORT void JNICALL
Java_agentflow_jni_NativeBridge_nativeFreeCancel(JNIEnv* /*env*/,
                                                 jobject /*self*/,
                                                 jlong cancel_id_j) {
  std::lock_guard<std::mutex> lk(g_cancel_mu);
  g_cancels.erase(cancel_id_j);
}

// ── Persistent session lifecycle (multi-turn) ────────────────────────────────

// Caches the JavaVM so the session worker thread can attach for callbacks.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_vm = vm;
  return JNI_VERSION_1_6;
}

// Creates a persistent session: loads the engine (model) ONCE, parses the
// workflow, and starts a worker thread driving the session's io_context. The
// conversation is created lazily on the first sendMessage. Returns an opaque
// session id, or throws on failure (e.g. engine create / workflow parse).
//
// Java signature:
//   external fun nativeCreateSession(modelPath: String, workflowJson: String): Long
JNIEXPORT jlong JNICALL
Java_agentflow_jni_NativeBridge_nativeCreateSession(
    JNIEnv* env, jobject /*self*/,
    jstring model_path_j,
    jstring workflow_json_j) {
  try {
    const std::string model_path    = JString(env, model_path_j).str();
    const std::string workflow_json = JString(env, workflow_json_j).str();

    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path});
    if (!engine) {
      ThrowJava(env, "LiteRtLmEngine::Create failed");
      return 0;
    }

    auto sess = std::make_shared<Session>();
    sess->engine = std::move(engine);
    sess->host_tools = std::make_shared<af::ToolRegistry>(sess->io);

    auto wf_or =
        af::workflow::WorkflowLoader::Load(workflow_json, *sess->host_tools);
    if (!wf_or.ok()) {
      ThrowJava(env, std::string(wf_or.status().message()).c_str());
      return 0;
    }
    sess->workflow = *wf_or;

    // Drive the io_context on a dedicated worker thread for the session's life.
    sess->worker = std::thread([sess]() { sess->io.run(); });

    std::lock_guard<std::mutex> lk(g_session_mu);
    jlong id = g_next_session_id++;
    g_sessions[id] = std::move(sess);
    return id;
  } catch (const std::exception& e) {
    ThrowJava(env, e.what());
    return 0;
  } catch (...) {
    ThrowJava(env, "unknown C++ exception");
    return 0;
  }
}

// Sends one user message on a persistent session and streams the reply deltas
// to onToken. Reuses the session's engine + conversation (multi-turn). Blocks
// until the turn completes, then returns the full assistant reply.
//
// Java signature:
//   external fun nativeSessionSendMessage(
//       sessionId: Long, userQuery: String, onToken: TokenCallback, cancelId: Long): String
JNIEXPORT jstring JNICALL
Java_agentflow_jni_NativeBridge_nativeSessionSendMessage(
    JNIEnv* env, jobject /*self*/,
    jlong session_id_j,
    jstring user_query_j,
    jobject on_token_j,
    jlong cancel_id_j) {
  auto sess = LookupSession(session_id_j);
  if (!sess) {
    ThrowJava(env, "invalid session id");
    return nullptr;
  }
  // One decode at a time per session (the conversation is not concurrent-safe).
  std::lock_guard<std::mutex> send_lk(sess->send_mu);

  try {
    const std::string user_query = JString(env, user_query_j).str();

    ::agentflow::CancelToken cancel_tok;
    if (cancel_id_j != 0) {
      std::lock_guard<std::mutex> lk(g_cancel_mu);
      auto it = g_cancels.find(cancel_id_j);
      if (it != g_cancels.end()) cancel_tok = it->second->Token();
    }

    // The token callback is invoked from the session WORKER thread (the
    // consumer coroutine runs on sess->io), so we must hold a GLOBAL ref to the
    // callback object — a local ref like `on_token_j` is only valid on this JNI
    // calling thread and using it from another thread crashes. jmethodID is
    // thread-independent and stays valid.
    jmethodID on_token_mid = nullptr;
    jobject on_token_global = nullptr;
    if (on_token_j != nullptr) {
      jclass cb_cls = env->GetObjectClass(on_token_j);
      on_token_mid =
          env->GetMethodID(cb_cls, "onToken", "(Ljava/lang/String;)V");
      env->DeleteLocalRef(cb_cls);
      if (on_token_mid == nullptr) {
        ThrowJava(env, "callback missing onToken(String) method");
        return nullptr;
      }
      on_token_global = env->NewGlobalRef(on_token_j);
    }

    // Build a fresh single-turn graph that REUSES the session's engine and
    // persistent conversation slot (so history carries across messages). Each
    // generated delta is marshalled onto the JVM callback inline by on_delta,
    // which runs on the session WORKER thread (sess->io) — so it uses a global
    // ref plus that thread's attached JNIEnv (`tenv`, set when the run
    // coroutine starts, before any delta can arrive).
    JNIEnv* tenv = nullptr;
    bool cb_failed = false;
    auto on_delta = [&](std::string_view tok) {
      if (on_token_global == nullptr || tenv == nullptr || cb_failed) return;
      jstring js = tenv->NewStringUTF(std::string(tok).c_str());
      tenv->CallVoidMethod(on_token_global, on_token_mid, js);
      tenv->DeleteLocalRef(js);
      // Match the prior session behaviour: clear a throwing callback's
      // exception and stop forwarding for the rest of the turn.
      if (tenv->ExceptionCheck()) {
        tenv->ExceptionClear();
        cb_failed = true;
      }
    };

    af::workflow::AgentNodeBuildSpec build_spec;
    build_spec.workflow      = sess->workflow;
    build_spec.agent_name    = sess->workflow->spec().main();
    build_spec.host_tools    = sess->host_tools;
    build_spec.engine        = sess->engine;
    build_spec.io_ctx        = &sess->io;
    build_spec.input_field   = "user_query";
    build_spec.output_field  = "assistant_reply";
    build_spec.max_iter      = 5;
    build_spec.token_sink    = on_delta;
    auto built = af::workflow::BuildAgentNode(build_spec);
    if (built.cfg.system_prompt.empty() && !built.cfg.engine) {
      ThrowJava(env, "main agent not in roster");
      return nullptr;
    }
    // Keep sub-agent runtime / delegate tools alive for the whole session, not
    // just this message (rebuilt each call is fine; just retain the latest).
    sess->keepalive = std::move(built.keepalive);

    built.cfg.constrained_tool_calls = false;
    // Reuse the persistent conversation across messages.
    built.cfg.conversation_slot = &sess->conversation;

    af::GraphBuilder b;
    b.AddNode(std::make_unique<af::AgentNode>(std::move(built.cfg)))
     .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
     .AddEdge("agent", "sink");
    auto graph = b.Build();

    af::test::TestState init;
    init.set_user_query(user_query);
    af::Runner runner(std::move(graph), af::Runner::Options{});

    std::string reply;
    std::exception_ptr run_exc;

    // One coroutine on the session worker thread: attach it to the JVM for the
    // turn (so on_delta can marshal each delta inline — no separate consumer),
    // run the graph, then detach. The JNI calling thread blocks on the latch
    // until the turn finishes, since the worker thread drives sess->io, not us.
    std::mutex done_mu;
    std::condition_variable done_cv;
    bool done = false;
    asio::co_spawn(sess->io, [&]() -> asio::awaitable<void> {
      bool attached = false;
      if (on_token_global != nullptr && g_vm != nullptr) {
        if (g_vm->GetEnv(reinterpret_cast<void**>(&tenv), JNI_VERSION_1_6) !=
            JNI_OK) {
          g_vm->AttachCurrentThread(reinterpret_cast<void**>(&tenv), nullptr);
          attached = true;
        }
      }
      try {
        auto out = co_await runner.Run(af::State::From(init), cancel_tok);
        reply = out.As<af::test::TestState>().assistant_reply();
      } catch (...) {
        run_exc = std::current_exception();
      }
      tenv = nullptr;  // no more deltas; stop on_delta touching the JVM
      if (attached) g_vm->DetachCurrentThread();
      {
        std::lock_guard<std::mutex> lk(done_mu);
        done = true;
      }
      done_cv.notify_one();
      co_return;
    }, asio::detached);

    // Wait for the turn to finish (the worker thread drives sess->io).
    {
      std::unique_lock<std::mutex> lk(done_mu);
      done_cv.wait(lk, [&] { return done; });
    }

    if (on_token_global != nullptr) env->DeleteGlobalRef(on_token_global);
    if (run_exc) std::rethrow_exception(run_exc);
    return env->NewStringUTF(reply.c_str());
  } catch (const std::exception& e) {
    ThrowJava(env, e.what());
    return nullptr;
  } catch (...) {
    ThrowJava(env, "unknown C++ exception");
    return nullptr;
  }
}

// Tears down a persistent session: cancels any in-flight decode, stops the
// worker thread, and frees the engine/conversation. Safe to call once.
//
// Java signature:
//   external fun nativeCloseSession(sessionId: Long)
JNIEXPORT void JNICALL
Java_agentflow_jni_NativeBridge_nativeCloseSession(JNIEnv* /*env*/,
                                                   jobject /*self*/,
                                                   jlong session_id_j) {
  std::shared_ptr<Session> sess;
  {
    std::lock_guard<std::mutex> lk(g_session_mu);
    auto it = g_sessions.find(session_id_j);
    if (it == g_sessions.end()) return;
    sess = std::move(it->second);
    g_sessions.erase(it);
  }
  // Cancel the in-flight turn (permanently) and let the worker drain.
  if (sess->conversation) sess->conversation->Cancel();
  // Release the work guard so io.run() returns, then join the worker.
  sess->work_guard.reset();
  if (sess->worker.joinable()) sess->worker.join();
  // engine / conversation / workflow destroyed when sess refcount drops here.
}

}  // extern "C"
