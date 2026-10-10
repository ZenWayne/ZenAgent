#include "agentflow/skills/skill_tools.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "agentflow/skills/skill_library.h"
#include "tests/unit/skills/skill_test_util.h"

namespace agentflow::skills {
namespace {
namespace fs = std::filesystem;
using testing_util::FreshTempDir;
using testing_util::SkillMd;
using testing_util::WriteFile;

std::string InvokeTool(Tool& tool, const std::string& args) {
  asio::io_context io;
  auto future = asio::co_spawn(io, [&]() -> asio::awaitable<std::string> {
    co_return co_await tool.Invoke(args, "call-1", CancelToken{});
  }, asio::use_future);
  io.run();
  return future.get();
}

std::string ErrorCode(const std::string& result) {
  auto parsed = nlohmann::json::parse(result, nullptr, false);
  if (!parsed.is_object() || !parsed.contains("message") ||
      !parsed["message"].is_string() || !parsed.contains("error") ||
      !parsed["error"].is_string()) return "";
  return parsed["error"].get<std::string>();
}

class SkillToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = FreshTempDir("tools");
    WriteFile(root_ / "alpha" / "SKILL.md",
              SkillMd("alpha", "Alpha does A.", "# Alpha\nSee references/guide.md.\n"));
    WriteFile(root_ / "alpha" / "references" / "guide.md", "guide text");
    WriteFile(root_ / "alpha" / "scripts" / "run.sh", "echo run");
    WriteFile(root_ / "beta" / "SKILL.md", SkillMd("beta", "Beta does B.", ""));
    WriteFile(root_ / "hidden" / "SKILL.md", SkillMd("hidden", "not visible", "secret"));
    WriteFile(root_ / "outside.txt", "outside");
    auto lib = SkillLibrary::Load({{root_, "builtin"}});
    ASSERT_TRUE(lib.ok()) << lib.status();
    lib_ = *lib;
    tools_ = MakeSkillTools(lib_, {"alpha", "beta"});
    ASSERT_EQ(tools_.size(), 2u);
  }
  Tool& load() { return *tools_[0]; }
  Tool& read() { return *tools_[1]; }

  fs::path root_;
  std::shared_ptr<const SkillLibrary> lib_;
  std::vector<std::shared_ptr<Tool>> tools_;
};

TEST_F(SkillToolsTest, SchemasEnumerateOnlyVisibleSkills) {
  EXPECT_EQ(load().Schema().name, "load_skill");
  EXPECT_EQ(read().Schema().name, "read_skill_file");
  auto load_schema = nlohmann::json::parse(load().Schema().params_json_schema);
  EXPECT_EQ(load_schema["properties"]["name"]["enum"],
            nlohmann::json::array({"alpha", "beta"}));
  EXPECT_EQ(load_schema["required"], nlohmann::json::array({"name"}));
  EXPECT_EQ(load_schema["additionalProperties"], false);
  auto read_schema = nlohmann::json::parse(read().Schema().params_json_schema);
  EXPECT_EQ(read_schema["properties"]["skill"]["enum"],
            nlohmann::json::array({"alpha", "beta"}));
  EXPECT_EQ(read_schema["required"], nlohmann::json::array({"skill", "path"}));
}

TEST_F(SkillToolsTest, LoadSkillReturnsLiteralBodyWithDirectoryHint) {
  EXPECT_EQ(InvokeTool(load(), R"({"name":"alpha"})"),
            "Skill directory: alpha/ — use read_skill_file(skill, path) for files "
            "referenced below.\n\n# Alpha\nSee references/guide.md.\n");
  EXPECT_EQ(InvokeTool(load(), R"({"name":"beta"})"),
            "Skill directory: beta/ — use read_skill_file(skill, path) for files "
            "referenced below.\n\n");
}

TEST_F(SkillToolsTest, InvisibleUnknownAndMalformedSkillsReturnStructuredErrors) {
  EXPECT_EQ(ErrorCode(InvokeTool(load(), R"({"name":"hidden"})")), "unknown_skill");
  EXPECT_EQ(ErrorCode(InvokeTool(load(), R"({"name":"nope"})")), "unknown_skill");
  EXPECT_EQ(ErrorCode(InvokeTool(load(), R"({})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(load(), R"({"name":2})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(load(), "not json")), "bad_args");
  auto error = nlohmann::json::parse(InvokeTool(load(), R"({"name":"nope"})"));
  EXPECT_NE(error["message"].get<std::string>().find("alpha, beta"), std::string::npos);
}

TEST_F(SkillToolsTest, ReadsTextIncludingScriptsWithoutExecutingThem) {
  EXPECT_EQ(InvokeTool(read(), R"({"skill":"alpha","path":"references/guide.md"})"),
            "guide text");
  EXPECT_EQ(InvokeTool(read(), R"({"skill":"alpha","path":"scripts/run.sh"})"), "echo run");
  EXPECT_NE(InvokeTool(read(), R"({"skill":"alpha","path":"SKILL.md"})").find("name: alpha"),
            std::string::npos);
}

TEST_F(SkillToolsTest, RejectsAbsoluteTraversalAndSymlinkEscape) {
  for (const char* path : {"../outside.txt", "references/../SKILL.md", "../hidden/SKILL.md"}) {
    EXPECT_EQ(ErrorCode(InvokeTool(read(), nlohmann::json{{"skill", "alpha"}, {"path", path}}.dump())),
              "path_not_allowed");
  }
  EXPECT_EQ(ErrorCode(InvokeTool(read(), nlohmann::json{{"skill", "alpha"},
      {"path", (root_ / "outside.txt").string()}}.dump())), "path_not_allowed");
  fs::create_symlink(root_ / "outside.txt", root_ / "alpha" / "link.txt");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":"link.txt"})")),
            "path_not_allowed");
  fs::create_directory_symlink(root_, root_ / "alpha" / "escape");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":"escape/outside.txt"})")),
            "path_not_allowed");
}

TEST_F(SkillToolsTest, RejectsInvalidTargetsAndMalformedArguments) {
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":""})")), "path_not_allowed");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":"references"})")), "not_a_file");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":"nope.md"})")), "not_found");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"hidden","path":"SKILL.md"})")),
            "unknown_skill");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha"})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":4})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":null,"path":"SKILL.md"})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(read(), "not json")), "bad_args");
}

TEST_F(SkillToolsTest, EnforcesByteLimitAndStrictUtf8) {
  WriteFile(root_ / "alpha" / "max.txt", std::string(kMaxSkillFileBytes, 'x'));
  EXPECT_EQ(InvokeTool(read(), R"({"skill":"alpha","path":"max.txt"})").size(),
            kMaxSkillFileBytes);
  WriteFile(root_ / "alpha" / "big.txt", std::string(kMaxSkillFileBytes + 1, 'x'));
  EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":"big.txt"})")), "too_large");
  WriteFile(root_ / "alpha" / "cn.md", "中文内容");
  EXPECT_EQ(InvokeTool(read(), R"({"skill":"alpha","path":"cn.md"})"), "中文内容");
  for (const std::string& invalid : {std::string("\xff\xfe", 2),
                                    std::string("\xc0\xaf", 2),
                                    std::string("\xed\xa0\x80", 3),
                                    std::string("\xf4\x90\x80\x80", 4)}) {
    WriteFile(root_ / "alpha" / "invalid.bin", invalid);
    EXPECT_EQ(ErrorCode(InvokeTool(read(), R"({"skill":"alpha","path":"invalid.bin"})")),
              "not_utf8");
  }
}

TEST(MakeSkillTools, EmptyVisibilityMeansNoTools) {
  auto root = FreshTempDir("tools");
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok());
  EXPECT_TRUE(MakeSkillTools(*lib, {}).empty());
}

TEST_F(SkillToolsTest, RejectsUnknownVisibleNamesBeforeCreatingTools) {
  EXPECT_THROW(MakeSkillTools(lib_, {"alpha", "missing"}), std::invalid_argument);
  EXPECT_THROW(MakeSkillTools(lib_, {"alpha", "alpha"}), std::invalid_argument);
}

TEST(RenderSkillCatalog, EmptyForNoSkills) {
  EXPECT_EQ(RenderSkillCatalog({}), "");
}

TEST(RenderSkillCatalog, ListsNameAndSingleLineDescription) {
  Skill a;
  a.name = "alpha";
  a.description = "Alpha does A.";
  Skill b;
  b.name = "beta";
  b.description = "Beta\nspans  lines.\n";
  EXPECT_EQ(RenderSkillCatalog({&a, &b}),
            "## 技能\n"
            "以下技能按需加载：判断当前任务用得上时，先调用 load_skill(name) 读完全文再动手。\n"
            "- alpha: Alpha does A.\n"
            "- beta: Beta spans lines.\n");
}

}  // namespace
}  // namespace agentflow::skills
