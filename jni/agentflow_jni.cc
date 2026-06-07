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
#include <exception>
#include <memory>
#include <string>
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
    jobject on_token_j) {
  try {
    const std::string model_path    = JString(env, model_path_j).str();
    const std::string workflow_json = JString(env, workflow_json_j).str();
    const std::string user_query    = JString(env, user_query_j).str();

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
    std::vector<std::shared_ptr<void>> keepalive = std::move(built.keepalive);

    // Wire the direct token stream + force the streaming (unconstrained) path.
    af::TokenChannel channel(io, /*capacity=*/4096);
    built.cfg.stream_tokens = true;
    built.cfg.constrained_tool_calls = false;
    built.cfg.token_channel = &channel;

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
        auto out = co_await runner.Run(af::State::From(init));
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

}  // extern "C"
