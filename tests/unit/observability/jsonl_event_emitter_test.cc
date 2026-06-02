// tests/unit/observability/jsonl_event_emitter_test.cc
#include "agentflow/observability/jsonl_event_emitter.h"

#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow {
namespace {

using json = nlohmann::json;

TEST(JsonlEventEmitterTest, OneLinePerEvent) {
  std::stringstream out;
  JsonlEventEmitter emit(out);
  emit.EmitToken("llm", "hi");
  emit.EmitToken("llm", "there");

  std::string s = out.str();
  EXPECT_EQ(std::count(s.begin(), s.end(), '\n'), 2);
}

TEST(JsonlEventEmitterTest, EventParsesAsJsonWithExpectedFields) {
  std::stringstream out;
  JsonlEventEmitter emit(out);
  emit.EmitToken("planner", "hello");

  std::string line = out.str();
  ASSERT_FALSE(line.empty());
  ASSERT_EQ(line.back(), '\n');
  line.pop_back();  // strip trailing newline before parse
  auto parsed = json::parse(line);
  // kind enum is rendered as its name in protobuf JSON.
  EXPECT_EQ(parsed.value("kind", ""), "TOKEN");
  EXPECT_EQ(parsed.value("nodeId", ""), "planner");
  ASSERT_TRUE(parsed.contains("token"));
  EXPECT_EQ(parsed["token"].value("token", ""), "hello");
}

TEST(JsonlEventEmitterTest, ThreadSafeUnderConcurrentEmit) {
  std::stringstream out;
  JsonlEventEmitter emit(out);
  constexpr int kPerThread = 500;
  constexpr int kThreads = 4;

  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&emit, t] {
      for (int i = 0; i < kPerThread; ++i) {
        emit.EmitToken("n" + std::to_string(t), "x");
      }
    });
  }
  for (auto& th : ts) th.join();

  std::string s = out.str();
  int newlines = std::count(s.begin(), s.end(), '\n');
  EXPECT_EQ(newlines, kThreads * kPerThread);

  // Every line must parse independently (no interleaving).
  std::stringstream ss(s);
  std::string line;
  int parsed = 0;
  while (std::getline(ss, line)) {
    if (line.empty()) continue;
    ASSERT_NO_THROW(json::parse(line)) << "bad line: " << line;
    ++parsed;
  }
  EXPECT_EQ(parsed, kThreads * kPerThread);
}

}  // namespace
}  // namespace agentflow
