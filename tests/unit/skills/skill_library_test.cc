#include "agentflow/skills/skill_library.h"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tests/unit/skills/skill_test_util.h"

namespace agentflow::skills {
namespace {

using testing_util::FreshTempDir;
using testing_util::SkillMd;
using testing_util::WriteFile;

TEST(SkillLibrary, LoadsEverySubdirectoryWithSkillMd) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "alpha" / "SKILL.md", SkillMd("alpha", "A", "alpha body"));
  WriteFile(root / "beta" / "SKILL.md", SkillMd("beta", "B", "beta body"));
  WriteFile(root / "beta" / "scripts" / "run.sh", "echo hi");
  std::filesystem::create_directories(root / "not-a-skill");
  WriteFile(root / "README.md", "loose file ignored");

  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto all = (*lib)->All();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0]->name, "alpha");
  EXPECT_EQ(all[1]->name, "beta");
  EXPECT_FALSE(all[0]->has_scripts);
  EXPECT_TRUE(all[1]->has_scripts);
  EXPECT_EQ(all[1]->dir, std::filesystem::canonical(root / "beta"));
  EXPECT_EQ((*lib)->Find("alpha")->body, "alpha body");
  EXPECT_EQ((*lib)->Find("gamma"), nullptr);
}

TEST(SkillLibrary, MissingRootIsEmptyNotAnError) {
  auto root = FreshTempDir("lib");
  auto lib = SkillLibrary::Load({{root / "does-not-exist", "user"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  EXPECT_TRUE((*lib)->All().empty());
}

TEST(SkillLibrary, RootThatIsAFileIsAnError) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "file", "x");
  EXPECT_FALSE(SkillLibrary::Load({{root / "file", "user"}}).ok());
}

TEST(SkillLibrary, LaterRootOverridesEarlierAndWarns) {
  auto low = FreshTempDir("low");
  auto high = FreshTempDir("high");
  WriteFile(low / "s" / "SKILL.md", SkillMd("s", "low", "low body"));
  WriteFile(high / "s" / "SKILL.md", SkillMd("s", "high", "high body"));
  std::vector<std::string> warnings;
  auto lib = SkillLibrary::Load({{low, "builtin"}, {high, "user"}}, &warnings);
  ASSERT_TRUE(lib.ok()) << lib.status();
  EXPECT_EQ((*lib)->Find("s")->description, "high");
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("overrides"), std::string::npos);
}

TEST(SkillLibrary, UnknownFrontmatterFieldsBecomeWarnings) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "s" / "SKILL.md", "---\nname: s\ndescription: d\nmodel: x\n---\n");
  std::vector<std::string> warnings;
  ASSERT_TRUE(SkillLibrary::Load({{root, "builtin"}}, &warnings).ok());
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("model"), std::string::npos);
}

TEST(SkillLibrary, BrokenSkillFailsWholeLoadWithItsPath) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "good" / "SKILL.md", SkillMd("good", "d", ""));
  WriteFile(root / "bad" / "SKILL.md", SkillMd("other-name", "d", ""));
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_FALSE(lib.ok());
  EXPECT_NE(lib.status().message().find("builtin"), std::string::npos);
  EXPECT_NE(lib.status().message().find("bad"), std::string::npos);
}

TEST(SkillLibrary, BrokenOverrideDoesNotFallBackToLowerRoot) {
  auto low = FreshTempDir("low");
  auto high = FreshTempDir("high");
  WriteFile(low / "s" / "SKILL.md", SkillMd("s", "fine", ""));
  WriteFile(high / "s" / "SKILL.md", "---\nname: s\n---\n");
  auto lib = SkillLibrary::Load({{low, "builtin"}, {high, "user"}});
  ASSERT_FALSE(lib.ok());
  EXPECT_NE(lib.status().message().find("user"), std::string::npos);
}

TEST(SkillLibrary, SelectKeepsRequestedOrder) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "a" / "SKILL.md", SkillMd("a", "d", ""));
  WriteFile(root / "b" / "SKILL.md", SkillMd("b", "d", ""));
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok());
  auto sel = (*lib)->Select({"b", "a"});
  ASSERT_TRUE(sel.ok()) << sel.status();
  ASSERT_EQ(sel->size(), 2u);
  EXPECT_EQ((*sel)[0]->name, "b");
  EXPECT_EQ((*sel)[1]->name, "a");
}

TEST(SkillLibrary, SelectUnknownIsNotFound) {
  auto root = FreshTempDir("lib");
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok());
  auto sel = (*lib)->Select({"missing"});
  ASSERT_FALSE(sel.ok());
  EXPECT_EQ(sel.status().code(), absl::StatusCode::kNotFound);
  EXPECT_NE(sel.status().message().find("missing"), std::string::npos);
}

TEST(SkillLibrary, SelectDuplicateIsInvalidArgument) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "a" / "SKILL.md", SkillMd("a", "d", ""));
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok());
  auto sel = (*lib)->Select({"a", "a"});
  ASSERT_FALSE(sel.ok());
  EXPECT_EQ(sel.status().code(), absl::StatusCode::kInvalidArgument);
}

}  // namespace
}  // namespace agentflow::skills
