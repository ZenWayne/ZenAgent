// tests/unit/core/json_text_test.cc
#include "agentflow/core/json_text.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow {
namespace {

TEST(JsonTextTest, KeepsValidUtf8Verbatim) {
  nlohmann::json j = {{"text", "搜索失败：尾帧"}};
  std::string out = DumpJsonLenient(j);
  EXPECT_NE(out.find("搜索失败：尾帧"), std::string::npos);
  EXPECT_EQ(nlohmann::json::parse(out), j);
}

TEST(JsonTextTest, MatchesStrictDumpForValidInput) {
  nlohmann::json j = {{"role", "tool"}, {"content", {{{"x", 1}}}}};
  EXPECT_EQ(DumpJsonLenient(j), j.dump());
}

TEST(JsonTextTest, ReplacesInvalidUtf8InsteadOfThrowing) {
  // A lone continuation-start byte, as produced by a split multi-byte char.
  nlohmann::json j = {{"error", std::string("parse error near \xE5' tail")}};
  ASSERT_THROW(j.dump(), nlohmann::json::type_error);

  std::string out;
  EXPECT_NO_THROW(out = DumpJsonLenient(j));
  nlohmann::json back = nlohmann::json::parse(out);
  EXPECT_NE(back["error"].get<std::string>().find("parse error near"),
            std::string::npos);
  EXPECT_NE(back["error"].get<std::string>().find("' tail"), std::string::npos);
}

}  // namespace
}  // namespace agentflow
