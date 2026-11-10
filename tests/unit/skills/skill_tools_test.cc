#include "agentflow/skills/skill_tools.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
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
using testing_util::SkillMdWithTools;
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

std::string Hint(std::string_view skill) {
  return "Skill directory: " + std::string(skill) +
         "/ — referenced files are loaded with load_skill(name=\"" +
         std::string(skill) + "\", path=\"<file>\").";
}

class SkillToolsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = FreshTempDir("tools");
    WriteFile(root_ / "alpha" / "SKILL.md",
              SkillMdWithTools("alpha", "Alpha does A.", "# Alpha\nBody.\n",
                               {"get_shot", "update_motion"}));
    WriteFile(root_ / "alpha" / "references" / "guide.md", "guide text");
    WriteFile(root_ / "alpha" / "scripts" / "run.sh", "echo run");
    WriteFile(root_ / "alpha" / "室内灯光参考.md",
              "---\ntools: [get_shot]\ndescription: 灯光\n---\n# 灯光\n暖色\n");
    WriteFile(root_ / "alpha" / "notes.md", "plain notes");
    WriteFile(root_ / "alpha" / "alpha-only.md", "alpha only");
    WriteFile(root_ / "alpha" / "dup.md", "alpha dup");
    WriteFile(root_ / "beta" / "SKILL.md",
              SkillMd("beta", "Beta does B.", "Beta body.\n"));
    WriteFile(root_ / "beta" / "dup.md", "beta dup");
    WriteFile(root_ / "hidden" / "SKILL.md",
              SkillMd("hidden", "not visible", "secret"));
    WriteFile(root_ / "outside.txt", "outside");
    auto lib = SkillLibrary::Load({{root_, "builtin"}});
    ASSERT_TRUE(lib.ok()) << lib.status();
    lib_ = *lib;
  }

  std::vector<std::shared_ptr<Tool>> MakeTools(
      std::vector<std::string> visible = {"alpha", "beta"},
      SkillLoadHook on_load = {}) {
    auto tools = MakeSkillTools(lib_, std::move(visible), std::move(on_load));
    EXPECT_TRUE(tools.ok()) << tools.status();
    if (!tools.ok()) return {};
    return *tools;
  }

  Tool& Named(std::vector<std::shared_ptr<Tool>>& tools, std::string_view name) {
    for (auto& tool : tools) {
      if (tool->Schema().name == name) return *tool;
    }
    ADD_FAILURE() << "missing tool " << name;
    return *tools.front();
  }

  fs::path root_;
  std::shared_ptr<const SkillLibrary> lib_;
};

TEST_F(SkillToolsTest, SchemasExposeFindLoadAndReadTools) {
  auto tools = MakeTools();
  ASSERT_EQ(tools.size(), 3u);
  auto& load = Named(tools, "load_skill");
  auto& find = Named(tools, "find_skill");
  auto& read = Named(tools, "read_skill_file");

  const auto load_schema = nlohmann::json::parse(load.Schema().params_json_schema);
  EXPECT_EQ(load_schema["required"], nlohmann::json::array({"name"}));
  EXPECT_TRUE(load_schema["properties"].contains("path"));
  EXPECT_FALSE(load_schema["properties"]["name"].contains("enum"));
  EXPECT_EQ(load_schema["additionalProperties"], false);

  const auto find_schema = nlohmann::json::parse(find.Schema().params_json_schema);
  EXPECT_EQ(find_schema["required"], nlohmann::json::array({"query"}));
  EXPECT_EQ(find_schema["properties"]["limit"]["minimum"], 1);
  EXPECT_FALSE(find_schema["properties"].contains("query") &&
               find_schema["properties"]["query"].contains("enum"));

  const auto read_schema = nlohmann::json::parse(read.Schema().params_json_schema);
  EXPECT_EQ(read_schema["properties"]["skill"]["enum"],
            nlohmann::json::array({"alpha", "beta"}));
  EXPECT_EQ(read_schema["required"], nlohmann::json::array({"skill", "path"}));
}

TEST_F(SkillToolsTest, FindSkillSearchesVisibleSkillsAndFiles) {
  auto tools = MakeTools();
  auto& find = Named(tools, "find_skill");

  const auto by_name = nlohmann::json::parse(InvokeTool(find, R"({"query":"alpha"})"));
  ASSERT_EQ(by_name["skills"].size(), 1u);
  EXPECT_EQ(by_name["skills"][0]["name"], "alpha");
  EXPECT_EQ(by_name["skills"][0]["description"], "Alpha does A.");
  EXPECT_EQ(by_name["skills"][0]["matched"], "");

  const auto hidden = nlohmann::json::parse(InvokeTool(find, R"({"query":"hidden"})"));
  EXPECT_TRUE(hidden["skills"].empty());

  const auto by_file =
      nlohmann::json::parse(InvokeTool(find, R"({"query":"室内灯光参考.md"})"));
  ASSERT_EQ(by_file["skills"].size(), 1u);
  EXPECT_EQ(by_file["skills"][0]["name"], "alpha");
  EXPECT_EQ(by_file["skills"][0]["matched"], "室内灯光参考.md");

  const auto limited =
      nlohmann::json::parse(InvokeTool(find, R"({"query":"alpha","limit":1})"));
  EXPECT_EQ(limited["skills"].size(), 1u);
}

TEST_F(SkillToolsTest, FindSkillRejectsMalformedArguments) {
  auto tools = MakeTools();
  auto& find = Named(tools, "find_skill");
  EXPECT_EQ(ErrorCode(InvokeTool(find, "{}")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(find, R"({"query":2})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(find, R"({"query":"  "})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(find, R"({"query":"a","limit":0})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(find, R"({"query":"a","limit":"x"})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(find, "not json")), "bad_args");
}

TEST_F(SkillToolsTest, LoadSkillReturnsHintBodyAndRegisteredTools) {
  std::vector<SkillLoadResult> seen;
  auto tools = MakeTools({"alpha", "beta"},
                         [&seen](const SkillLoadResult& result) {
                           seen.push_back(result);
                           return absl::OkStatus();
                         });
  auto& load = Named(tools, "load_skill");
  const std::string text = InvokeTool(load, R"({"name":"alpha"})");
  EXPECT_EQ(text, Hint("alpha") +
                      "\n\n# Alpha\nBody.\n\n"
                      "Registered tools: get_shot, update_motion");
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].text, text);
  EXPECT_EQ(seen[0].loaded_skill, "alpha");
  EXPECT_TRUE(seen[0].loaded_file.empty());
  EXPECT_FALSE(seen[0].is_file);
  EXPECT_EQ(seen[0].tool_names,
            (std::vector<std::string>{"get_shot", "update_motion"}));
}

TEST_F(SkillToolsTest, LoadSkillWithoutAHookIsReadOnly) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  const std::string text = InvokeTool(load, R"({"name":"alpha"})");
  EXPECT_EQ(text, Hint("alpha") + "\n\n# Alpha\nBody.\n");
  EXPECT_EQ(text.find("Registered tools"), std::string::npos);
}

TEST_F(SkillToolsTest, SkillWithoutToolsHasNoRegisteredToolsLine) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  const std::string text = InvokeTool(load, R"({"name":"beta"})");
  EXPECT_EQ(text, Hint("beta") + "\n\nBeta body.\n");
  EXPECT_EQ(text.find("Registered tools"), std::string::npos);
  EXPECT_EQ(text.back(), '\n');
  EXPECT_NE(text[text.size() - 2], '\n');
}

TEST_F(SkillToolsTest, LoadSkillByPathStripsReferenceFrontmatter) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  EXPECT_EQ(InvokeTool(load, R"({"name":"alpha","path":"室内灯光参考.md"})"),
            Hint("alpha") + "\n\n# 灯光\n暖色\n");
  EXPECT_EQ(InvokeTool(load, R"({"name":"alpha","path":"notes.md"})"),
            Hint("alpha") + "\n\nplain notes");
  EXPECT_EQ(InvokeTool(load, R"({"name":"alpha","path":"references/guide.md"})"),
            Hint("alpha") + "\n\nguide text");
}

TEST_F(SkillToolsTest, ReferenceToolsArePassedToTheHook) {
  std::vector<SkillLoadResult> seen;
  auto tools = MakeTools({"alpha", "beta"},
                         [&seen](const SkillLoadResult& result) {
                           seen.push_back(result);
                           return absl::OkStatus();
                         });
  auto& load = Named(tools, "load_skill");
  EXPECT_EQ(InvokeTool(load, R"({"name":"alpha","path":"室内灯光参考.md"})"),
            Hint("alpha") + "\n\n# 灯光\n暖色\n\nRegistered tools: get_shot");
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_TRUE(seen[0].is_file);
  EXPECT_EQ(seen[0].loaded_skill, "alpha");
  EXPECT_EQ(seen[0].loaded_file, "室内灯光参考.md");
  EXPECT_EQ(seen[0].tool_names, (std::vector<std::string>{"get_shot"}));
}

TEST_F(SkillToolsTest, CurrentSkillLoadsBareFileNamesFirst) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  ASSERT_EQ(InvokeTool(load, R"({"name":"alpha"})"),
            Hint("alpha") + "\n\n# Alpha\nBody.\n");
  EXPECT_EQ(InvokeTool(load, R"({"name":"dup.md"})"),
            Hint("alpha") + "\n\nalpha dup");
  ASSERT_EQ(InvokeTool(load, R"({"name":"beta"})"),
            Hint("beta") + "\n\nBeta body.\n");
  EXPECT_EQ(InvokeTool(load, R"({"name":"dup.md"})"),
            Hint("beta") + "\n\nbeta dup");
  // Loading a reference file never moves the current skill.
  ASSERT_TRUE(InvokeTool(load, R"({"name":"alpha","path":"notes.md"})")
                  .find("plain notes") != std::string::npos);
  EXPECT_EQ(InvokeTool(load, R"({"name":"dup.md"})"),
            Hint("beta") + "\n\nbeta dup");
}

TEST_F(SkillToolsTest, UniqueFileNamesResolveAcrossVisibleSkills) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  EXPECT_EQ(InvokeTool(load, R"({"name":"alpha-only.md"})"),
            Hint("alpha") + "\n\nalpha only");
}

TEST_F(SkillToolsTest, AmbiguousAndUnknownFilesReportCandidates) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  const std::string ambiguous = InvokeTool(load, R"({"name":"dup.md"})");
  EXPECT_EQ(ErrorCode(ambiguous), "ambiguous_skill_file");
  const auto parsed = nlohmann::json::parse(ambiguous);
  const std::string message = parsed["message"].get<std::string>();
  EXPECT_NE(message.find("alpha"), std::string::npos);
  EXPECT_NE(message.find("beta"), std::string::npos);

  const std::string unknown = InvokeTool(load, R"({"name":"missing.md"})");
  EXPECT_EQ(ErrorCode(unknown), "unknown_skill");
  const std::string unknown_message =
      nlohmann::json::parse(unknown)["message"].get<std::string>();
  EXPECT_NE(unknown_message.find("alpha, beta"), std::string::npos);
}

TEST_F(SkillToolsTest, LoadSkillEnforcesThePathBoundary) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  for (const char* path : {"../outside.txt", "references/../SKILL.md"}) {
    EXPECT_EQ(ErrorCode(InvokeTool(
                  load, nlohmann::json{{"name", "alpha"}, {"path", path}}.dump())),
              "path_not_allowed");
  }
  EXPECT_EQ(ErrorCode(InvokeTool(
                load, nlohmann::json{{"name", "alpha"},
                                     {"path", (root_ / "outside.txt").string()}}
                          .dump())),
            "path_not_allowed");
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"alpha","path":""})")),
            "path_not_allowed");
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"alpha","path":"references"})")),
            "not_a_file");
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"alpha","path":"nope.md"})")),
            "not_found");
  EXPECT_EQ(
      ErrorCode(InvokeTool(load, R"({"name":"hidden","path":"SKILL.md"})")),
      "unknown_skill");
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"alpha","path":4})")),
            "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(load, "not json")), "bad_args");

  fs::create_symlink(root_ / "outside.txt", root_ / "alpha" / "link.txt");
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"alpha","path":"link.txt"})")),
            "path_not_allowed");
}

TEST_F(SkillToolsTest, LoadSkillEnforcesByteAndUtf8Limits) {
  auto tools = MakeTools();
  auto& load = Named(tools, "load_skill");
  WriteFile(root_ / "alpha" / "max.txt", std::string(kMaxSkillFileBytes, 'x'));
  EXPECT_EQ(InvokeTool(load, R"({"name":"alpha","path":"max.txt"})").size(),
            Hint("alpha").size() + 2 + kMaxSkillFileBytes);
  WriteFile(root_ / "alpha" / "big.txt",
            std::string(kMaxSkillFileBytes + 1, 'x'));
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"alpha","path":"big.txt"})")),
            "too_large");
  WriteFile(root_ / "alpha" / "invalid.bin", std::string("\xff\xfe", 2));
  EXPECT_EQ(
      ErrorCode(InvokeTool(load, R"({"name":"alpha","path":"invalid.bin"})")),
      "not_utf8");
}

TEST_F(SkillToolsTest, HookFailureFailsTheWholeLoadWithoutABody) {
  std::vector<SkillLoadResult> seen;
  auto tools = MakeTools(
      {"alpha", "beta"}, [&seen](const SkillLoadResult& result) {
        seen.push_back(result);
        if (result.is_file) return absl::OkStatus();
        return SkillToolError("skill_tool_ungraded",
                              "get_shot is not graded for this agent");
      });
  auto& load = Named(tools, "load_skill");
  const std::string text = InvokeTool(load, R"({"name":"alpha"})");
  EXPECT_EQ(ErrorCode(text), "skill_tool_ungraded");
  const auto parsed = nlohmann::json::parse(text);
  const std::string message = parsed["message"].get<std::string>();
  EXPECT_NE(message.find("not graded"), std::string::npos);
  EXPECT_EQ(message.find("# Alpha"), std::string::npos);
  ASSERT_EQ(seen.size(), 1u);

  // The failed load must not become the current skill.
  EXPECT_EQ(ErrorCode(InvokeTool(load, R"({"name":"dup.md"})")),
            "ambiguous_skill_file");
}

TEST_F(SkillToolsTest, PlainHookErrorsBecomeToolRegistrationFailed) {
  auto tools = MakeTools({"alpha", "beta"},
                         [](const SkillLoadResult&) {
                           return absl::InternalError("backend exploded");
                         });
  auto& load = Named(tools, "load_skill");
  const std::string text = InvokeTool(load, R"({"name":"alpha"})");
  EXPECT_EQ(ErrorCode(text), "tool_registration_failed");
  EXPECT_NE(text.find("backend exploded"), std::string::npos);
  EXPECT_EQ(text.find("# Alpha"), std::string::npos);
}

TEST_F(SkillToolsTest, RepeatedLoadsAreIdempotent) {
  int calls = 0;
  auto tools = MakeTools({"alpha", "beta"}, [&calls](const SkillLoadResult&) {
    ++calls;
    return absl::OkStatus();
  });
  auto& load = Named(tools, "load_skill");
  const std::string first = InvokeTool(load, R"({"name":"alpha"})");
  const std::string second = InvokeTool(load, R"({"name":"alpha"})");
  EXPECT_EQ(first, second);
  EXPECT_EQ(calls, 2);
}

TEST_F(SkillToolsTest, ReadSkillFileKeepsItsV1Behavior) {
  auto tools = MakeTools();
  auto& read = Named(tools, "read_skill_file");
  EXPECT_EQ(InvokeTool(read, R"({"skill":"alpha","path":"references/guide.md"})"),
            "guide text");
  EXPECT_EQ(InvokeTool(read, R"({"skill":"alpha","path":"scripts/run.sh"})"),
            "echo run");
  EXPECT_NE(InvokeTool(read, R"({"skill":"alpha","path":"SKILL.md"})")
                .find("name: alpha"),
            std::string::npos);
}

TEST_F(SkillToolsTest, ReadSkillFileKeepsItsV1Rejections) {
  auto tools = MakeTools();
  auto& read = Named(tools, "read_skill_file");
  for (const char* path : {"../outside.txt", "references/../SKILL.md",
                           "../hidden/SKILL.md"}) {
    EXPECT_EQ(ErrorCode(InvokeTool(
                  read, nlohmann::json{{"skill", "alpha"}, {"path", path}}.dump())),
              "path_not_allowed");
  }
  EXPECT_EQ(ErrorCode(InvokeTool(
                read, nlohmann::json{{"skill", "alpha"},
                                     {"path", (root_ / "outside.txt").string()}}
                          .dump())),
            "path_not_allowed");
  fs::create_symlink(root_ / "outside.txt", root_ / "alpha" / "read-link.txt");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"alpha","path":"read-link.txt"})")),
            "path_not_allowed");
  fs::create_directory_symlink(root_, root_ / "alpha" / "escape");
  EXPECT_EQ(ErrorCode(InvokeTool(
                read, R"({"skill":"alpha","path":"escape/outside.txt"})")),
            "path_not_allowed");

  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"alpha","path":""})")),
            "path_not_allowed");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"alpha","path":"references"})")),
            "not_a_file");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"alpha","path":"nope.md"})")),
            "not_found");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"hidden","path":"SKILL.md"})")),
            "unknown_skill");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"alpha"})")), "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":"alpha","path":4})")),
            "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(read, R"({"skill":null,"path":"SKILL.md"})")),
            "bad_args");
  EXPECT_EQ(ErrorCode(InvokeTool(read, "not json")), "bad_args");
}

TEST_F(SkillToolsTest, ReadSkillFileEnforcesByteAndUtf8Limits) {
  auto tools = MakeTools();
  auto& read = Named(tools, "read_skill_file");
  WriteFile(root_ / "alpha" / "read-max.txt",
            std::string(kMaxSkillFileBytes, 'x'));
  EXPECT_EQ(InvokeTool(read, R"({"skill":"alpha","path":"read-max.txt"})").size(),
            kMaxSkillFileBytes);
  WriteFile(root_ / "alpha" / "read-big.txt",
            std::string(kMaxSkillFileBytes + 1, 'x'));
  EXPECT_EQ(
      ErrorCode(InvokeTool(read, R"({"skill":"alpha","path":"read-big.txt"})")),
      "too_large");
  WriteFile(root_ / "alpha" / "cn.md", "中文内容");
  EXPECT_EQ(InvokeTool(read, R"({"skill":"alpha","path":"cn.md"})"), "中文内容");
  for (const std::string& invalid :
       {std::string("\xff\xfe", 2), std::string("\xc0\xaf", 2),
        std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4)}) {
    WriteFile(root_ / "alpha" / "invalid-read.bin", invalid);
    EXPECT_EQ(ErrorCode(InvokeTool(
                  read, R"({"skill":"alpha","path":"invalid-read.bin"})")),
              "not_utf8");
  }
}

TEST(MakeSkillTools, EmptyVisibilityMeansNoTools) {
  auto root = FreshTempDir("tools");
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok());
  auto tools = MakeSkillTools(*lib, {});
  ASSERT_TRUE(tools.ok()) << tools.status();
  EXPECT_TRUE(tools->empty());
  auto no_library = MakeSkillTools(nullptr, {});
  ASSERT_TRUE(no_library.ok()) << no_library.status();
  EXPECT_TRUE(no_library->empty());
}

TEST_F(SkillToolsTest, RejectsUnknownVisibleNamesBeforeCreatingTools) {
  auto missing = MakeSkillTools(lib_, {"alpha", "missing"});
  EXPECT_EQ(missing.status().code(), absl::StatusCode::kNotFound);
  EXPECT_NE(missing.status().message().find("missing"), std::string::npos);
  auto duplicate = MakeSkillTools(lib_, {"alpha", "alpha"});
  EXPECT_EQ(duplicate.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(duplicate.status().message().find("alpha"), std::string::npos);
}

TEST(MakeSkillTools, NullLibraryReturnsAnErrorWithoutThrowing) {
  auto tools = MakeSkillTools(nullptr, {"alpha"});
  EXPECT_EQ(tools.status().code(), absl::StatusCode::kInvalidArgument);
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
            "## Skills\n"
            "Load a skill on demand: when it applies to the task, call load_skill(name) "
            "and read its full instructions before acting.\n"
            "- alpha: Alpha does A.\n"
            "- beta: Beta spans lines.\n");
}

}  // namespace
}  // namespace agentflow::skills
