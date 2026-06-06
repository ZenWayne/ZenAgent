#include "agentflow/workflow/json_path.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace agentflow::workflow {
namespace {

TEST(JsonPathTest, RootField) {
  auto path_or = JsonPath::Parse("$.answer");
  ASSERT_TRUE(path_or.ok()) << path_or.status();
  nlohmann::ordered_json j = {{"answer", "42"}};
  auto v = path_or->Resolve(j);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->get<std::string>(), "42");
}

TEST(JsonPathTest, NestedFieldAndArray) {
  auto path_or = JsonPath::Parse("$.content[0].text");
  ASSERT_TRUE(path_or.ok());
  auto j = nlohmann::ordered_json::parse(R"({"content":[{"text":"hello"},{"text":"world"}]})");
  auto v = path_or->Resolve(j);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->get<std::string>(), "hello");
}

TEST(JsonPathTest, MissingPathReturnsEmpty) {
  auto path_or = JsonPath::Parse("$.nope");
  ASSERT_TRUE(path_or.ok());
  EXPECT_FALSE(path_or->Resolve(nlohmann::ordered_json::object()).has_value());
}

TEST(JsonPathTest, ArrayIndexOutOfRange) {
  auto path_or = JsonPath::Parse("$.arr[5]");
  ASSERT_TRUE(path_or.ok());
  auto j = nlohmann::ordered_json::parse(R"({"arr":[1,2,3]})");
  EXPECT_FALSE(path_or->Resolve(j).has_value());
}

TEST(JsonPathTest, RejectMissingDollarPrefix) {
  EXPECT_FALSE(JsonPath::Parse("content[0]").ok());
}

TEST(JsonPathTest, RejectUnbalancedBracket) {
  EXPECT_FALSE(JsonPath::Parse("$.arr[0").ok());
}

}  // namespace
}  // namespace agentflow::workflow
