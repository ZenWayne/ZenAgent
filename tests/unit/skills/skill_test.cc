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

TEST(ParseSkillMd, UnicodeLengthLimitsCountCharacters) {
  const std::string max_description = [] {
    std::string value;
    for (int i = 0; i < 1024; ++i) value += "é";
    return value;
  }();
  auto description_ok = ParseSkillMd(
      "---\nname: x\ndescription: \"" + max_description + "\"\n---\n", "x");
  ASSERT_TRUE(description_ok.ok()) << description_ok.status();
  auto description_too_long = ParseSkillMd(
      "---\nname: x\ndescription: \"" + max_description + "é\"\n---\n", "x");
  EXPECT_FALSE(description_too_long.ok());

  const std::string max_compatibility = [] {
    std::string value;
    for (int i = 0; i < 500; ++i) value += "é";
    return value;
  }();
  auto compatibility_ok = ParseSkillMd(
      "---\nname: x\ndescription: d\ncompatibility: \"" + max_compatibility +
          "\"\n---\n",
      "x");
  ASSERT_TRUE(compatibility_ok.ok()) << compatibility_ok.status();
  auto compatibility_too_long = ParseSkillMd(
      "---\nname: x\ndescription: d\ncompatibility: \"" + max_compatibility +
          "é\"\n---\n",
      "x");
  EXPECT_FALSE(compatibility_too_long.ok());
}

TEST(ParseSkillMd, RejectsInvalidUtf8InLimitedFields) {
  const std::string invalid_utf8("\xC0\xAF", 2);
  auto invalid_description = ParseSkillMd(
      "---\nname: x\ndescription: \"" + invalid_utf8 + "\"\n---\n", "x");
  EXPECT_FALSE(invalid_description.ok());
  auto invalid_compatibility = ParseSkillMd(
      "---\nname: x\ndescription: d\ncompatibility: \"" + invalid_utf8 +
          "\"\n---\n",
      "x");
  EXPECT_FALSE(invalid_compatibility.ok());
}

TEST(ParseSkillMd, RejectsMalformedUtf8Body) {
  const std::vector<std::string> malformed = {
      "\xFF",          // Invalid leading byte.
      "\x80",          // Stray continuation byte.
      "\xC0\xAF",      // Overlong encoding.
      "\xE2\x82",      // Truncated sequence.
      "\xE2\x28\xA1",  // Invalid continuation byte.
      "\xED\xA0\x80",  // UTF-16 surrogate.
      "\xF4\x90\x80\x80",  // Above U+10FFFF.
  };
  for (const auto& bytes : malformed) {
    auto s = ParseSkillMd(
        "---\nname: x\ndescription: d\n---\nvalid prefix\n" + bytes, "x");
    ASSERT_FALSE(s.ok());
    EXPECT_NE(s.status().message().find("body"), std::string::npos);
    EXPECT_NE(s.status().message().find("UTF-8"), std::string::npos);
  }
}

TEST(ParseSkillMd, PreservesUnicodeBodyAfterNormalization) {
  auto s = ParseSkillMd(
      "\xEF\xBB\xBF---\r\nname: x\r\ndescription: d\r\n---\r\n\r\n"
      "# 台词 café 😀\r\n正文 {{state.x}}\r\n", "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->body, "# 台词 café 😀\n正文 {{state.x}}\n");
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
namespace agentflow::skills {
namespace {

TEST(ParseSkillMd, ParsesToolList) {
  auto s = ParseSkillMd(
      "---\nname: x\ndescription: d\ntools:\n  - get_shot\n  - update_motion\n---\n",
      "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->tools,
            (std::vector<std::string>{"get_shot", "update_motion"}));
  EXPECT_TRUE(s->allowed_tools.empty());
  EXPECT_FALSE(s->tools_from_allowed_tools);
}

TEST(ParseSkillMd, ToolsAreDedupedAndCappedAtSixtyFour) {
  auto deduped = ParseSkillMd(
      "---\nname: x\ndescription: d\ntools: [a, a, b]\n---\n", "x");
  ASSERT_TRUE(deduped.ok()) << deduped.status();
  EXPECT_EQ(deduped->tools, (std::vector<std::string>{"a", "b"}));

  std::string list;
  for (int i = 0; i < 64; ++i) list += "  - t" + std::to_string(i) + "\n";
  auto max_tools = ParseSkillMd(
      "---\nname: x\ndescription: d\ntools:\n" + list + "---\n", "x");
  ASSERT_TRUE(max_tools.ok()) << max_tools.status();
  EXPECT_EQ(max_tools->tools.size(), 64u);

  auto too_many = ParseSkillMd(
      "---\nname: x\ndescription: d\ntools:\n" + list + "  - t64\n---\n", "x");
  ASSERT_FALSE(too_many.ok());
  EXPECT_NE(too_many.status().message().find("64"), std::string::npos);
}

TEST(ParseSkillMd, RejectsMalformedToolLists) {
  EXPECT_FALSE(
      ParseSkillMd("---\nname: x\ndescription: d\ntools: get_shot\n---\n", "x")
          .ok());
  EXPECT_FALSE(
      ParseSkillMd("---\nname: x\ndescription: d\ntools: [\"\"]\n---\n", "x")
          .ok());
  EXPECT_FALSE(
      ParseSkillMd("---\nname: x\ndescription: d\ntools: {a: b}\n---\n", "x")
          .ok());
}

TEST(ParseSkillMd, AllowedToolsIsAMigrationAliasForTools) {
  std::vector<std::string> unknown;
  auto s = ParseSkillMd(
      "---\nname: x\ndescription: d\n"
      "allowed-tools: get_shot  update_dialogue\n---\n",
      "x", &unknown);
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->allowed_tools,
            (std::vector<std::string>{"get_shot", "update_dialogue"}));
  EXPECT_EQ(s->tools, s->allowed_tools);
  EXPECT_TRUE(s->tools_from_allowed_tools);
  EXPECT_TRUE(unknown.empty());
}

TEST(ParseSkillMd, ExplicitToolsWinOverAllowedToolsWithoutTheAlias) {
  auto s = ParseSkillMd(
      "---\nname: x\ndescription: d\ntools: [a, b]\nallowed-tools: c\n---\n",
      "x");
  ASSERT_TRUE(s.ok()) << s.status();
  EXPECT_EQ(s->tools, (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(s->allowed_tools, (std::vector<std::string>{"c"}));
  EXPECT_FALSE(s->tools_from_allowed_tools);
}

TEST(ParseReferenceMd, PlainTextFileIsTheWholeBody) {
  auto ref = ParseReferenceMd("# 灯光\n正文\n", "notes.md");
  ASSERT_TRUE(ref.ok()) << ref.status();
  EXPECT_EQ(ref->name, "notes");
  EXPECT_TRUE(ref->description.empty());
  EXPECT_TRUE(ref->tools.empty());
  EXPECT_EQ(ref->body, "# 灯光\n正文\n");
}

TEST(ParseReferenceMd, ParsesToolsAndDescription) {
  auto ref = ParseReferenceMd(
      "---\ntools: [get_shot, update_shot]\ndescription: 灯光参考\n---\n"
      "# 灯光\n暖色\n",
      "室内灯光参考.md");
  ASSERT_TRUE(ref.ok()) << ref.status();
  EXPECT_EQ(ref->name, "室内灯光参考");
  EXPECT_EQ(ref->description, "灯光参考");
  EXPECT_EQ(ref->tools, (std::vector<std::string>{"get_shot", "update_shot"}));
  EXPECT_EQ(ref->body, "# 灯光\n暖色\n");
}

TEST(ParseReferenceMd, DefaultsTheNameToTheFilenameWithoutMd) {
  auto ref = ParseReferenceMd("body", "shot-grammar.md");
  ASSERT_TRUE(ref.ok()) << ref.status();
  EXPECT_EQ(ref->name, "shot-grammar");
  auto other = ParseReferenceMd("body", "notes.txt");
  ASSERT_TRUE(other.ok()) << other.status();
  EXPECT_EQ(other->name, "notes.txt");
}

TEST(ParseReferenceMd, KeepingAnUnclosedFrontmatterAsBody) {
  const std::string text = "---\ntools: [a]\nnot closed\n";
  auto ref = ParseReferenceMd(text, "broken.md");
  ASSERT_TRUE(ref.ok()) << ref.status();
  EXPECT_TRUE(ref->tools.empty());
  EXPECT_EQ(ref->body, text);
}

TEST(ParseReferenceMd, RejectsMalformedToolLists) {
  EXPECT_FALSE(ParseReferenceMd("---\ntools: get_shot\n---\n", "x.md").ok());
  EXPECT_FALSE(ParseReferenceMd("---\ntools: [\"\"]\n---\n", "x.md").ok());
  EXPECT_FALSE(ParseReferenceMd("---\nname: \"\"\n---\n", "x.md").ok());
}

}  // namespace
}  // namespace agentflow::skills
