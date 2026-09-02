// tests/repro/multi_session_teardown.cc
//
// Bisect repro for docs/litert-lm-multi-session-teardown-bug.md.
//
//   capi       N conversation create/send/delete cycles through the raw C API
//              only. No agentflow wrappers, no asio, no extra threads.
//   framework  the same N cycles through LiteRtLmChatBackend, driven on an
//              io_context with a streaming token sink, dropping each
//              conversation before the next one is created.
//   overlap    what deep-search actually does: an outer ("planner")
//              conversation stays alive while N inner ("searcher")
//              conversations are created, used and destroyed underneath it.
//   capi-overlap  the same overlap shape through the raw C API only.
//
// If a `capi*` mode aborts, the defect is in the vendored engine. If the capi
// modes are clean and a framework mode aborts, the defect is in agentflow's
// wrapper layer.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/use_awaitable.hpp>

#include "agentflow/inference/chat_backend.h"
#include "agentflow/inference/litert_lm_chat_backend.h"
#include "agentflow/inference/litert_lm_engine.h"
#include "c/conversation.h"
#include "c/engine.h"

namespace {

const char* kPrompts[] = {
    "{\"role\":\"user\",\"content\":\"Say the word apple and nothing else.\"}",
    "{\"role\":\"user\",\"content\":\"Say the word banana and nothing else.\"}",
    "{\"role\":\"user\",\"content\":\"Say the word cherry and nothing else.\"}",
    "{\"role\":\"user\",\"content\":\"Say the word durian and nothing else.\"}",
    "{\"role\":\"user\",\"content\":\"Say the word elderberry and nothing "
    "else.\"}",
    "{\"role\":\"user\",\"content\":\"Say the word fig and nothing else.\"}",
};
constexpr int kNumPrompts = sizeof(kPrompts) / sizeof(kPrompts[0]);

int RunCApi(const std::string& model_path, int cycles) {
  auto* settings = litert_lm_engine_settings_create(model_path.c_str(), "cpu",
                                                    nullptr, nullptr);
  litert_lm_engine_settings_set_max_num_tokens(settings, 1024);
  auto* engine = litert_lm_engine_create(settings);
  litert_lm_engine_settings_delete(settings);
  if (!engine) {
    std::fprintf(stderr, "engine create failed\n");
    return 1;
  }
  for (int i = 0; i < cycles; ++i) {
    std::fprintf(stderr, "[capi] === cycle %d ===\n", i);
    auto* cfg = litert_lm_conversation_config_create();
    litert_lm_conversation_config_set_enable_constrained_decoding(cfg, false);
    if (!cfg) {
      std::fprintf(stderr, "[capi] cycle %d: config create FAILED\n", i);
      break;
    }
    auto* conv = litert_lm_conversation_create(engine, cfg);
    if (!conv) {
      std::fprintf(stderr, "[capi] cycle %d: conversation create FAILED\n", i);
      litert_lm_conversation_config_delete(cfg);
      break;
    }
    auto* resp = litert_lm_conversation_send_message(
        conv, kPrompts[i % kNumPrompts], nullptr, nullptr);
    if (resp) {
      const char* s = litert_lm_json_response_get_string(resp);
      std::fprintf(stderr, "[capi] cycle %d reply: %.120s\n", i, s ? s : "");
      litert_lm_json_response_delete(resp);
    } else {
      std::fprintf(stderr, "[capi] cycle %d: send FAILED\n", i);
    }
    litert_lm_conversation_delete(conv);
    litert_lm_conversation_config_delete(cfg);
    std::fprintf(stderr, "[capi] cycle %d torn down\n", i);
  }
  std::fprintf(stderr, "[capi] deleting engine\n");
  litert_lm_engine_delete(engine);
  std::fprintf(stderr, "[capi] engine deleted cleanly\n");
  return 0;
}

int RunFramework(const std::string& model_path, int cycles) {
  agentflow::LiteRtLmEngineOptions opts;
  opts.model_path = model_path;
  opts.max_num_tokens = 1024;
  auto engine = agentflow::LiteRtLmEngine::Create(opts);
  if (!engine) {
    std::fprintf(stderr, "engine create failed\n");
    return 1;
  }
  asio::io_context io;
  auto backend = agentflow::LiteRtLmChatBackend::Create(engine, io);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        for (int i = 0; i < cycles; ++i) {
          std::fprintf(stderr, "[framework] === cycle %d ===\n", i);
          agentflow::ChatConversationOptions copts;
          auto conv = backend->CreateConversation(std::move(copts));
          if (!conv) {
            std::fprintf(stderr, "[framework] cycle %d: create FAILED\n", i);
            break;
          }
          agentflow::CancelToken cancel;
          agentflow::TokenSink sink =
              [](std::string_view) -> asio::awaitable<void> { co_return; };
          auto r =
              co_await conv->SendAsync(kPrompts[i % kNumPrompts], sink, cancel);
          std::fprintf(stderr, "[framework] cycle %d reply ok=%d: %.120s\n", i,
                       static_cast<int>(r.ok()),
                       r.ok() ? r->c_str() : r.status().ToString().c_str());
          // Drop the conversation here — this is what AgentNode::Run does when
          // it returns, and it is what the repro is about.
          conv.reset();
          std::fprintf(stderr, "[framework] cycle %d torn down\n", i);
        }
        co_return;
      },
      asio::detached);

  io.run();
  std::fprintf(stderr, "[framework] io drained; releasing backend + engine\n");
  backend.reset();
  engine.reset();
  std::fprintf(stderr, "[framework] engine deleted cleanly\n");
  return 0;
}

// Raw C API, deep-search shape: an outer conversation stays alive across N
// inner conversation lifecycles.
int RunCApiOverlap(const std::string& model_path, int cycles) {
  auto* settings = litert_lm_engine_settings_create(model_path.c_str(), "cpu",
                                                    nullptr, nullptr);
  litert_lm_engine_settings_set_max_num_tokens(settings, 1024);
  auto* engine = litert_lm_engine_create(settings);
  litert_lm_engine_settings_delete(settings);
  if (!engine) {
    std::fprintf(stderr, "engine create failed\n");
    return 1;
  }

  auto* outer_cfg = litert_lm_conversation_config_create();
  litert_lm_conversation_config_set_enable_constrained_decoding(outer_cfg, false);
  auto* outer = litert_lm_conversation_create(engine, outer_cfg);
  std::fprintf(stderr, "[capi-overlap] outer conversation = %p\n",
               static_cast<void*>(outer));
  if (outer) {
    auto* resp = litert_lm_conversation_send_message(outer, kPrompts[0],
                                                     nullptr, nullptr);
    std::fprintf(stderr, "[capi-overlap] outer turn resp=%p\n",
                 static_cast<void*>(resp));
    if (resp) litert_lm_json_response_delete(resp);
  }

  for (int i = 0; i < cycles; ++i) {
    std::fprintf(stderr, "[capi-overlap] === inner %d (outer still alive) ===\n",
                 i);
    auto* cfg = litert_lm_conversation_config_create();
    litert_lm_conversation_config_set_enable_constrained_decoding(cfg, false);
    auto* conv = cfg ? litert_lm_conversation_create(engine, cfg) : nullptr;
    std::fprintf(stderr, "[capi-overlap] inner %d conv=%p\n", i,
                 static_cast<void*>(conv));
    if (conv) {
      auto* resp = litert_lm_conversation_send_message(
          conv, kPrompts[(i + 1) % kNumPrompts], nullptr, nullptr);
      if (resp) {
        const char* s = litert_lm_json_response_get_string(resp);
        std::fprintf(stderr, "[capi-overlap] inner %d reply: %.120s\n", i,
                     s ? s : "");
        litert_lm_json_response_delete(resp);
      } else {
        std::fprintf(stderr, "[capi-overlap] inner %d: send FAILED\n", i);
      }
      litert_lm_conversation_delete(conv);
    }
    if (cfg) litert_lm_conversation_config_delete(cfg);
    std::fprintf(stderr, "[capi-overlap] inner %d torn down\n", i);
  }

  std::fprintf(stderr, "[capi-overlap] deleting outer\n");
  if (outer) litert_lm_conversation_delete(outer);
  if (outer_cfg) litert_lm_conversation_config_delete(outer_cfg);
  std::fprintf(stderr, "[capi-overlap] deleting engine\n");
  litert_lm_engine_delete(engine);
  std::fprintf(stderr, "[capi-overlap] engine deleted cleanly\n");
  return 0;
}

// Framework, deep-search shape.
int RunOverlap(const std::string& model_path, int cycles) {
  agentflow::LiteRtLmEngineOptions opts;
  opts.model_path = model_path;
  opts.max_num_tokens = 1024;
  auto engine = agentflow::LiteRtLmEngine::Create(opts);
  if (!engine) {
    std::fprintf(stderr, "engine create failed\n");
    return 1;
  }
  asio::io_context io;
  auto backend = agentflow::LiteRtLmChatBackend::Create(engine, io);

  asio::co_spawn(
      io,
      [&]() -> asio::awaitable<void> {
        agentflow::CancelToken cancel;
        agentflow::TokenSink sink =
            [](std::string_view) -> asio::awaitable<void> { co_return; };

        auto outer = backend->CreateConversation({});
        std::fprintf(stderr, "[overlap] outer created ok=%d\n",
                     static_cast<int>(outer != nullptr));
        if (outer) {
          auto r = co_await outer->SendAsync(kPrompts[0], sink, cancel);
          std::fprintf(stderr, "[overlap] outer turn ok=%d\n",
                       static_cast<int>(r.ok()));
        }

        for (int i = 0; i < cycles; ++i) {
          std::fprintf(stderr, "[overlap] === inner %d (outer alive) ===\n", i);
          auto conv = backend->CreateConversation({});
          if (!conv) {
            std::fprintf(stderr, "[overlap] inner %d: create FAILED\n", i);
            continue;
          }
          auto r = co_await conv->SendAsync(kPrompts[(i + 1) % kNumPrompts],
                                            sink, cancel);
          std::fprintf(stderr, "[overlap] inner %d ok=%d: %.120s\n", i,
                       static_cast<int>(r.ok()),
                       r.ok() ? r->c_str() : r.status().ToString().c_str());
          conv.reset();
          std::fprintf(stderr, "[overlap] inner %d torn down\n", i);
        }
        outer.reset();
        std::fprintf(stderr, "[overlap] outer torn down\n");
        co_return;
      },
      asio::detached);

  io.run();
  std::fprintf(stderr, "[overlap] io drained; releasing backend + engine\n");
  backend.reset();
  engine.reset();
  std::fprintf(stderr, "[overlap] engine deleted cleanly\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* env_model = std::getenv("MODEL_PATH");
  std::string model = env_model ? env_model : "models/gemma-4-E2B-it.litertlm";
  std::string mode = argc > 1 ? argv[1] : "capi";
  int cycles = argc > 2 ? std::atoi(argv[2]) : 4;
  std::fprintf(stderr, "mode=%s cycles=%d model=%s\n", mode.c_str(), cycles,
               model.c_str());
  int rc;
  if (mode == "framework") {
    rc = RunFramework(model, cycles);
  } else if (mode == "overlap") {
    rc = RunOverlap(model, cycles);
  } else if (mode == "capi-overlap") {
    rc = RunCApiOverlap(model, cycles);
  } else {
    rc = RunCApi(model, cycles);
  }
  std::fprintf(stderr, "main returning %d\n", rc);
  return rc;
}
