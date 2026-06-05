// tests/unit/core/state_json_test.cc
#include "agentflow/core/state.h"

#include <nlohmann/json.hpp>
#include <gtest/gtest.h>

namespace agentflow {
namespace {

TEST(StateJsonTest, FromJsonProducesJsonKind) {
  nlohmann::ordered_json fields;
  fields["user_query"] = "";
  fields["counter"] = 0;
  State s = State::FromJson(fields);
  EXPECT_EQ(s.kind(), State::Kind::Json);
}

TEST(StateJsonTest, WriteAndReadStringField) {
  nlohmann::ordered_json fields;
  fields["greeting"] = nlohmann::ordered_json{{"type", "string"}};
  State s = State::FromJson(fields);
  WriteStringField(s, "greeting", "hello");
  EXPECT_EQ(ReadStringField(s, "greeting"), "hello");
}

TEST(StateJsonTest, NestedPathAutoCreates) {
  nlohmann::ordered_json fields;
  fields["nested"] = nlohmann::ordered_json{{"type", "object"}};
  State s = State::FromJson(fields);
  WriteStringField(s, "nested.inner", "x");
  EXPECT_EQ(ReadStringField(s, "nested.inner"), "x");
}

TEST(StateJsonTest, UndeclaredScratchpadAllowed) {
  State s = State::FromJson(nlohmann::ordered_json::object());
  WriteStringField(s, "tmp", "scratch");
  EXPECT_EQ(ReadStringField(s, "tmp"), "scratch");
}

TEST(StateJsonTest, ReadMissingReturnsEmpty) {
  State s = State::FromJson(nlohmann::ordered_json::object());
  EXPECT_EQ(ReadStringField(s, "nothing.here"), "");
}

}  // namespace
}  // namespace agentflow
