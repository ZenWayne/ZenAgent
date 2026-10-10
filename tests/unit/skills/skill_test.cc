#include "agentflow/skills/skill.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace agentflow::skills {
namespace {

TEST(IsValidSkillName, AcceptsStandardNames) {
  EXPECT_TRUE(IsValidSkillName("dialogue-authoring"));
  EXPECT_TRUE(IsValidSkillName("a2ui"));
  EXPECT_TRUE(IsValidSkillName(std::string(64, 'a')));
}

TEST(IsValidSkillName, RejectsNonStandardNames) {
  EXPECT_FALSE(IsValidSkillName(""));
  EXPECT_FALSE(IsValidSkillName(std::string(65, 'a')));
  EXPECT_FALSE(IsValidSkillName("Dialogue"));
  EXPECT_FALSE(IsValidSkillName("-lead"));
  EXPECT_FALSE(IsValidSkillName("trail-"));
  EXPECT_FALSE(IsValidSkillName("double--dash"));
  EXPECT_FALSE(IsValidSkillName("under_score"));
  EXPECT_FALSE(IsValidSkillName("台词"));
}

TEST(ParseSkillMd, ParsesAllStandardFields) {
  const std::string md =
      "---\n"
      "name: dialogue-authoring\n"
      "description: |\n"
      "  Use when writing dialogue.\n"
      "  Covers word counts.\n"
      "license: MIT\n"
      "compatibility: needs the video-maker MCP\n"
      "metadata:\n"
      "  author: wayne\n"
      "  version: \"1\"\n"
      "allowed-tools: get_shot  update_dialogue\n"
      "---\n"
      "\n"
      "# Title\n"
      "Body {{state.x}} stays literal.\n";
  std::vector<std::string> unknown;
  auto s = ParseSkillMd(md, "dialogue-authoring", &unknown);
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->name, "dialogue-authoring");
  EXPECT_EQ(s->description, "Use when writing dialogue.\nCovers word counts.\n");
  EXPECT_EQ(s->license, "MIT");
  EXPECT_EQ(s->compatibility, "needs the video-maker MCP");
  EXPECT_EQ(s->metadata.at("author"), "wayne");
  EXPECT_EQ(s->metadata.at("version"), "1");
  EXPECT_EQ(s->allowed_tools, (std::vector<std::string>{"get_shot", "update_dialogue"}));
  EXPECT_EQ(s->body, "# Title\nBody {{state.x}} stays literal.\n");
  EXPECT_TRUE(unknown.empty());
}

TEST(ParseSkillMd, AllowedToolsAlsoAcceptsAList) {
  auto s = ParseSkillMd("---\nname: x\ndescription: d\nallowed-tools: [a, b]\n---\n", "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->allowed_tools, (std::vector<std::string>{"a", "b"}));
}

TEST(ParseSkillMd, UnknownFieldsAreIgnoredButReported) {
  std::vector<std::string> unknown;
  auto s = ParseSkillMd("---\nname: x\ndescription: d\nmodel: opus\nhooks: {}\n---\nbody",
                        "x", &unknown);
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(unknown, (std::vector<std::string>{"model", "hooks"}));
}

TEST(ParseSkillMd, NameMustMatchDirectory) {
  auto s = ParseSkillMd("---\nname: x\ndescription: d\n---\n", "y");
  ASSERT_FALSE(s.ok());
  EXPECT_NE(s.status().message().find("directory"), std::string::npos);
}

TEST(ParseSkillMd, RejectsMissingOrInvalidRequiredFields) {
  EXPECT_FALSE(ParseSkillMd("---\ndescription: d\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\ndescription: \"  \"\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: Bad\ndescription: d\n---\n", "Bad").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\ndescription: [a]\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\ndescription: " + std::string(1025, 'd') + "\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\ndescription: d\ncompatibility: " + std::string(501, 'c') + "\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\ndescription: d\nmetadata: [a]\n---\n", "x").ok());
}

TEST(ParseSkillMd, RejectsMissingOrBrokenFrontmatter) {
  EXPECT_FALSE(ParseSkillMd("# no frontmatter\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: x\ndescription: d\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\nname: [x\n---\n", "x").ok());
  EXPECT_FALSE(ParseSkillMd("---\n---\nbody", "x").ok());
}

TEST(ParseSkillMd, AcceptsCrlfLineEndings) {
  auto s = ParseSkillMd("---\r\nname: x\r\ndescription: d\r\n---\r\nline1\r\nline2\r\n", "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->description, "d");
  EXPECT_EQ(s->body, "line1\nline2\n");
}

TEST(ParseSkillMd, AcceptsUtf8Bom) {
  auto s = ParseSkillMd("\xEF\xBB\xBF---\nname: x\ndescription: d\n---\nbody", "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->body, "body");
}

TEST(ParseSkillMd, AllowsEmptyBody) {
  auto s = ParseSkillMd("---\nname: x\ndescription: d\n---\n", "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->body, "");
}

}  // namespace
}  // namespace agentflow::skills
