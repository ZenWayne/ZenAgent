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
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>

#include "agentflow/core/cancel.h"
#include "agentflow/core/errors.h"
#include "agentflow/core/event.h"
#include "agentflow/core/graph.h"
#include "agentflow/core/runner.h"
#include "agentflow/core/state.h"
#include "agentflow/core/stub_node.h"
#include "agentflow/core/token_channel.h"
#include "agentflow/inference/litert_lm_chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "agentflow/nodes/agent_node.h"
#include "agentflow/tools/native_fn_tool.h"
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

// ── Host-tool upcall plumbing ──────────────────────────────────────────────

JavaVM* g_jvm = nullptr;

// Worker pool for JNI upcalls. A tool's Invoke must never run on the runner's
// single io thread (it would block the whole workflow, killing the parallel
// dispatch), so the blocking CallObjectMethod happens on these workers. Each
// worker attaches/detaches the JVM around its job.
struct UpcallWorkerPool {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  std::vector<std::thread> threads;
  bool shutdown = false;

  ~UpcallWorkerPool() {
    {
      std::lock_guard<std::mutex> lk(mu);
      shutdown = true;
    }
    cv.notify_all();
    for (auto& t : threads) {
      if (t.joinable()) t.join();
    }
  }

  void EnsureStarted(size_t n = 2) {
    std::lock_guard<std::mutex> lk(mu);
    if (!threads.empty()) return;
    for (size_t i = 0; i < n; ++i) {
      threads.emplace_back([this] {
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [this] { return shutdown || !jobs.empty(); });
            if (shutdown && jobs.empty()) return;
            job = std::move(jobs.front());
            jobs.pop_front();
          }
          job();
        }
      });
    }
  }

  void Post(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> lk(mu);
      jobs.push_back(std::move(job));
    }
    cv.notify_one();
  }
};

UpcallWorkerPool g_workers;

// Per-tool JVM call target (global ref owned by the registering run).
struct JvmToolTarget {
  jobject tool = nullptr;
  jmethodID invoke_mid = nullptr;
};

// Builds a NativeFnTool whose Fn performs the async JNI upcall: post the
// blocking invoke to a worker, await the result on a channel while yielding
// the runner thread, and flip the Kotlin-visible signal when the run's
// CancelToken fires.
std::shared_ptr<af::Tool> MakeHostTool(
    asio::io_context& io, std::string name, std::string description,
    std::string params_schema, JvmToolTarget target,
    jobject signal, jfieldID cancelled_fid) {
  using UpcallChannel =
      asio::experimental::concurrent_channel<void(asio::error_code, std::string)>;

  auto fn = [target, signal, cancelled_fid,
             &io](std::string_view args, std::string_view tool_call_id,
                  const af::CancelToken& cancel) -> asio::awaitable<std::string> {
    auto completion = std::make_shared<UpcallChannel>(io, 1);
    const std::string args_str(args);
    const std::string id_str(tool_call_id);

    g_workers.Post([target, signal, cancelled_fid, completion, args_str, id_str] {
      JNIEnv* wenv = nullptr;
      std::string result;
      if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&wenv),
                                     nullptr) == JNI_OK) {
        jstring id = wenv->NewStringUTF(id_str.c_str());
        jstring js_args = wenv->NewStringUTF(args_str.c_str());
        jstring js_result = static_cast<jstring>(
            wenv->CallObjectMethod(target.tool, target.invoke_mid, id, js_args,
                                   signal));
        if (wenv->ExceptionCheck()) {
          wenv->ExceptionClear();
          result = R"({"error":"tool_impl_threw"})";
        } else if (js_result != nullptr) {
          const char* cs = wenv->GetStringUTFChars(js_result, nullptr);
          result = cs ? std::string(cs) : R"({"error":"null_result"})";
          if (cs) wenv->ReleaseStringUTFChars(js_result, cs);
          wenv->DeleteLocalRef(js_result);
        } else {
          result = R"({"error":"null_result"})";
        }
        wenv->DeleteLocalRef(id);
        wenv->DeleteLocalRef(js_args);
        g_jvm->DetachCurrentThread();
      } else {
        result = R"({"error":"jvm_attach_failed"})";
      }
      asio::post(completion->get_executor(), [completion, result] {
        completion->try_send(asio::error_code{}, result);
      });
    });

    // Flip the Kotlin-visible signal when the run is cancelled, so tools
    // blocked on an approval gate (or long work) bail out promptly. The
    // OnCancel callback may fire on the runner thread — route the JNI field
    // write through the worker pool.
    cancel.OnCancel([signal, cancelled_fid] {
      g_workers.Post([signal, cancelled_fid] {
        JNIEnv* wenv = nullptr;
        if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&wenv),
                                       nullptr) == JNI_OK) {
          wenv->SetBooleanField(signal, cancelled_fid, JNI_TRUE);
          g_jvm->DetachCurrentThread();
        }
      });
    });

    // Yield the runner thread while the worker executes. A closed channel
    // (run unwinding) or receive error maps to the cancelled error slot.
    auto [ec, result] =
        co_await completion->async_receive(asio::as_tuple(asio::use_awaitable));
    if (ec) co_return std::string(R"({"error":"cancelled"})");
    co_return result;
  };

  return std::make_shared<af::NativeFnTool>(
      af::ToolSchema{std::move(name), std::move(description),
                     std::move(params_schema)},
      std::move(fn));
}

// Forwards tool lifecycle events to the Kotlin RunEventCallback. Events are
// emitted on the runner's io thread — the same thread the JNI call runs on —
// so env_ stays valid without AttachCurrentThread (same contract as the
// streaming token callback).
class JniEventEmitter : public af::EventEmitter {
 public:
  JniEventEmitter(JNIEnv* env, jobject events, jmethodID tool_call_mid,
                  jmethodID tool_return_mid)
      : env_(env), events_(events), tool_call_mid_(tool_call_mid),
        tool_return_mid_(tool_return_mid) {}

  void Emit(af::proto::TraceEvent ev) override {
    switch (ev.payload_case()) {
      case af::proto::TraceEvent::kToolCall: {
        const auto& p = ev.tool_call();
        jstring id = env_->NewStringUTF(p.tool_call_id().c_str());
        jstring name = env_->NewStringUTF(p.tool_name().c_str());
        jstring args = env_->NewStringUTF(p.args_json().c_str());
        env_->CallVoidMethod(events_, tool_call_mid_, id, name, args);
        env_->DeleteLocalRef(id);
        env_->DeleteLocalRef(name);
        env_->DeleteLocalRef(args);
        break;
      }
      case af::proto::TraceEvent::kToolReturn: {
        const auto& p = ev.tool_return();
        jstring id = env_->NewStringUTF(p.tool_call_id().c_str());
        jstring res = env_->NewStringUTF(p.result_json().c_str());
        env_->CallVoidMethod(events_, tool_return_mid_, id, res);
        env_->DeleteLocalRef(id);
        env_->DeleteLocalRef(res);
        break;
      }
      default:
        break;
    }
  }

 private:
  JNIEnv* env_;
  jobject events_;
  jmethodID tool_call_mid_;
  jmethodID tool_return_mid_;
};

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
    auto backend = af::LiteRtLmChatBackend::Create(engine, io);
    af::AgentNodeConfig agent_cfg;
    agent_cfg.backend = backend;
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
    auto backend = af::LiteRtLmChatBackend::Create(engine, io);
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
    build_spec.backend      = backend;
    build_spec.io_ctx       = &io;
    build_spec.input_field  = "user_query";
    build_spec.output_field = "assistant_reply";
    build_spec.max_iter     = 5;
    auto built = af::workflow::BuildAgentNode(build_spec);
    if (built.cfg.system_prompt.empty() && !built.cfg.backend) {
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
// streaming mode: each generated text delta is pushed onto a TokenChannel and
// forwarded to `onToken` as it arrives (a direct path, not the TraceEvent
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
    auto backend = af::LiteRtLmChatBackend::Create(engine, io);
    auto host_tools = std::make_shared<af::ToolRegistry>(io);

    auto wf_or =
        af::workflow::WorkflowLoader::Load(workflow_json, *host_tools);
    if (!wf_or.ok()) {
      ThrowJava(env, std::string(wf_or.status().message()).c_str());
      return nullptr;
    }
    auto wf = *wf_or;

    // Run-wide token stream, created BEFORE BuildAgentNode so it can be wired
    // into both the main agent and the delegate tool (each sub-agent gets its
    // own per-call channel that drains up to this one).
    af::TokenChannel channel(io, /*capacity=*/4096);

    af::workflow::AgentNodeBuildSpec build_spec;
    build_spec.workflow      = wf;
    build_spec.agent_name    = wf->spec().main();
    build_spec.host_tools    = host_tools;
    build_spec.backend       = backend;
    build_spec.io_ctx        = &io;
    build_spec.input_field   = "user_query";
    build_spec.output_field  = "assistant_reply";
    build_spec.max_iter      = 5;
    build_spec.token_channel = &channel;
    auto built = af::workflow::BuildAgentNode(build_spec);
    if (built.cfg.system_prompt.empty() && !built.cfg.backend) {
      ThrowJava(env, "main agent not in roster");
      return nullptr;
    }
    std::vector<std::shared_ptr<void>> keepalive = std::move(built.keepalive);

    // Streaming requires the unconstrained path (no streaming constrained C
    // bridge). BuildAgentNode already set stream_tokens + token_channel.
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

    // Producer side: run the graph, capture the reply, then close the channel
    // to signal the drain loop that the stream is finished.
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
      try {
        auto out = co_await runner.Run(af::State::From(init), cancel_tok);
        reply = out.As<af::test::TestState>().assistant_reply();
      } catch (...) {
        run_exc = std::current_exception();
      }
      channel.close();
      co_return;
    }, asio::detached);

    // Consumer side: forward each token to the JVM callback. Runs on the same
    // io thread as the JNI call, so `env` is valid for CallVoidMethod (no
    // AttachCurrentThread needed).
    asio::co_spawn(io, [&]() -> asio::awaitable<void> {
      for (;;) {
        auto [ec, tok] = co_await channel.async_receive(
            asio::as_tuple(asio::use_awaitable));
        if (ec) break;  // channel closed → end of stream
        if (on_token_mid != nullptr) {
          jstring js = env->NewStringUTF(tok.c_str());
          env->CallVoidMethod(on_token_j, on_token_mid, js);
          env->DeleteLocalRef(js);
          if (env->ExceptionCheck()) {
            // A Kotlin callback threw — stop forwarding; the exception stays
            // pending and surfaces to the caller when JNI returns.
            break;
          }
        }
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

// Java signature:
//   external fun runJsonWorkflowConstrained(
//       modelPath: String,
//       workflowJson: String,
//       tools: Array<HostTool>,
//       signal: CancellationSignal,
//       userQuery: String,
//       onEvent: RunEventCallback,
//       cancelId: Long,
//   ): String
//
// Constrained tool-mode run. Kotlin tools are registered into the host
// registry (their names must match the workflow JSON's "tools" entries — the
// loader rejects unknown names); the main agent runs with constrained
// tool-call decoding (no token stream — the constrained C bridge has no
// streaming variant), tool lifecycle events stream to the callback, and the
// full assistant reply is returned when the run completes.
JNIEXPORT jstring JNICALL
Java_agentflow_jni_NativeBridge_runJsonWorkflowConstrained(
    JNIEnv* env, jobject /*self*/,
    jstring model_path_j,
    jstring workflow_json_j,
    jobjectArray tools_j,
    jobject signal_j,
    jstring user_query_j,
    jobject events_j,
    jlong cancel_id_j) {
  try {
    const std::string model_path    = JString(env, model_path_j).str();
    const std::string workflow_json = JString(env, workflow_json_j).str();
    const std::string user_query    = JString(env, user_query_j).str();

    // Event callback methods (default-bodied Kotlin interface methods).
    jmethodID tool_call_mid = nullptr, tool_return_mid = nullptr;
    if (events_j != nullptr) {
      jclass cb_cls = env->GetObjectClass(events_j);
      tool_call_mid = env->GetMethodID(
          cb_cls, "onToolCall",
          "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
      tool_return_mid = env->GetMethodID(
          cb_cls, "onToolReturn",
          "(Ljava/lang/String;Ljava/lang/String;)V");
      if (tool_call_mid == nullptr || tool_return_mid == nullptr) {
        ThrowJava(env, "callback missing onToolCall/onToolReturn methods");
        return nullptr;
      }
    }

    // Cancel token (same registry as the streaming entry).
    ::agentflow::CancelToken cancel_tok;
    if (cancel_id_j != 0) {
      std::lock_guard<std::mutex> lk(g_cancel_mu);
      auto it = g_cancels.find(cancel_id_j);
      if (it != g_cancels.end()) cancel_tok = it->second->Token();
    }

    // RunSignal's `cancelled` field, flipped on cancel (see MakeHostTool).
    jobject signal_global = nullptr;
    jfieldID cancelled_fid = nullptr;
    if (signal_j != nullptr) {
      signal_global = env->NewGlobalRef(signal_j);
      jclass sig_cls = env->GetObjectClass(signal_j);
      cancelled_fid = env->GetFieldID(sig_cls, "cancelled", "Z");
      if (cancelled_fid == nullptr) {
        env->DeleteGlobalRef(signal_global);
        ThrowJava(env, "signal missing cancelled field");
        return nullptr;
      }
    }

    auto engine = af::LiteRtLmEngine::Create(
        af::LiteRtLmEngineOptions{.model_path = model_path});
    if (!engine) {
      if (signal_global != nullptr) env->DeleteGlobalRef(signal_global);
      ThrowJava(env, "LiteRtLmEngine::Create failed");
      return nullptr;
    }

    asio::io_context io;
    auto backend = af::LiteRtLmChatBackend::Create(engine, io);
    g_workers.EnsureStarted();

    // Register Kotlin tools into the run's host registry. Global refs are
    // owned by this run and released on every exit path below.
    auto host_tools = std::make_shared<af::ToolRegistry>(io);
    std::vector<jobject> tool_globals;
    if (tools_j != nullptr) {
      const jsize n = env->GetArrayLength(tools_j);
      for (jsize i = 0; i < n; ++i) {
        jobject tool = env->GetObjectArrayElement(tools_j, i);
        if (tool == nullptr) continue;
        jclass tool_cls = env->GetObjectClass(tool);
        auto read_field = [&](const char* getter) -> std::string {
          jmethodID mid =
              env->GetMethodID(tool_cls, getter, "()Ljava/lang/String;");
          if (mid == nullptr) return std::string{};
          jstring v = static_cast<jstring>(env->CallObjectMethod(tool, mid));
          std::string s = v != nullptr ? JString(env, v).str() : std::string{};
          if (v != nullptr) env->DeleteLocalRef(v);
          return s;
        };
        const std::string name = read_field("getName");
        const std::string description = read_field("getDescription");
        const std::string schema = read_field("getParamsJsonSchema");
        jmethodID invoke_mid = env->GetMethodID(
            tool_cls, "invoke",
            "(Ljava/lang/String;Ljava/lang/String;"
            "Lagentflow/dsl/CancellationSignal;)Ljava/lang/String;");
        if (invoke_mid == nullptr) {
          env->DeleteLocalRef(tool);
          for (jobject g : tool_globals) env->DeleteGlobalRef(g);
          if (signal_global != nullptr) env->DeleteGlobalRef(signal_global);
          ThrowJava(env, "HostTool missing invoke method");
          return nullptr;
        }
        jobject tool_global = env->NewGlobalRef(tool);
        tool_globals.push_back(tool_global);
        env->DeleteLocalRef(tool);
        host_tools->Register(MakeHostTool(
            io, name, description, schema,
            JvmToolTarget{tool_global, invoke_mid}, signal_global,
            cancelled_fid));
      }
    }

    auto wf_or =
        af::workflow::WorkflowLoader::Load(workflow_json, *host_tools);
    if (!wf_or.ok()) {
      for (jobject g : tool_globals) env->DeleteGlobalRef(g);
      if (signal_global != nullptr) env->DeleteGlobalRef(signal_global);
      ThrowJava(env, std::string(wf_or.status().message()).c_str());
      return nullptr;
    }
    auto wf = *wf_or;

    // No token channel; constrained_tool_calls stays as loaded from the
    // workflow JSON (the streaming entry forces it off — this one doesn't).
    af::workflow::AgentNodeBuildSpec build_spec;
    build_spec.workflow     = wf;
    build_spec.agent_name   = wf->spec().main();
    build_spec.host_tools   = host_tools;
    build_spec.backend      = backend;
    build_spec.io_ctx       = &io;
    build_spec.input_field  = "user_query";
    build_spec.output_field = "assistant_reply";
    build_spec.max_iter     = 5;
    auto built = af::workflow::BuildAgentNode(build_spec);
    if (built.cfg.system_prompt.empty() && !built.cfg.backend) {
      for (jobject g : tool_globals) env->DeleteGlobalRef(g);
      if (signal_global != nullptr) env->DeleteGlobalRef(signal_global);
      ThrowJava(env, "main agent not in roster");
      return nullptr;
    }
    std::vector<std::shared_ptr<void>> keepalive = std::move(built.keepalive);

    af::GraphBuilder b;
    b.AddNode(std::make_unique<af::AgentNode>(std::move(built.cfg)))
     .AddNode(std::make_unique<af::StubNode>("sink", 0ms, nullptr, nullptr))
     .AddEdge("agent", "sink");
    auto graph = b.Build();

    af::test::TestState init;
    init.set_user_query(user_query);
    JniEventEmitter emitter(env, events_j, tool_call_mid, tool_return_mid);
    af::Runner runner(std::move(graph),
                      af::Runner::Options{.trace = &emitter});

    std::string reply;
    std::exception_ptr run_exc;
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

    for (jobject g : tool_globals) env->DeleteGlobalRef(g);
    if (signal_global != nullptr) env->DeleteGlobalRef(signal_global);

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
                                                jobject /*self*/) {  std::lock_guard<std::mutex> lk(g_cancel_mu);
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

// Caches the JavaVM for the host-tool upcall worker pool.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_jvm = vm;
  return JNI_VERSION_1_6;
}

}  // extern "C"
