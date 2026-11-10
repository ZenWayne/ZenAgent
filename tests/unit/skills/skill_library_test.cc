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

TEST(SkillLibrary, MalformedUtf8BodyFailsOverrideWithoutFallback) {
  auto low = FreshTempDir("low");
  auto high = FreshTempDir("high");
  WriteFile(low / "s" / "SKILL.md", SkillMd("s", "fine", "valid body"));
  const auto malformed_path = high / "s" / "SKILL.md";
  WriteFile(malformed_path, SkillMd("s", "fine", "invalid body\n\xFF"));

  auto lib = SkillLibrary::Load({{low, "builtin"}, {high, "user"}});

  ASSERT_FALSE(lib.ok());
  EXPECT_EQ(lib.status().code(), absl::StatusCode::kInvalidArgument);
  EXPECT_NE(lib.status().message().find("user"), std::string::npos);
  EXPECT_NE(lib.status().message().find(malformed_path.string()), std::string::npos);
  EXPECT_NE(lib.status().message().find("body"), std::string::npos);
  EXPECT_NE(lib.status().message().find("UTF-8"), std::string::npos);
}

TEST(SkillLibrary, DirectoryManifestFailsOverrideWithoutFallback) {
  auto low = FreshTempDir("low");
  auto high = FreshTempDir("high");
  WriteFile(low / "s" / "SKILL.md", SkillMd("s", "fine", ""));
  std::filesystem::create_directories(high / "s" / "SKILL.md");

  auto lib = SkillLibrary::Load({{low, "builtin"}, {high, "user"}});

  ASSERT_FALSE(lib.ok());
  EXPECT_NE(lib.status().message().find("user"), std::string::npos);
  EXPECT_NE(lib.status().message().find("SKILL.md"), std::string::npos);
}

TEST(SkillLibrary, DanglingManifestSymlinkFailsOverrideWithoutFallback) {
  auto low = FreshTempDir("low");
  auto high = FreshTempDir("high");
  WriteFile(low / "s" / "SKILL.md", SkillMd("s", "fine", ""));
  std::filesystem::create_directories(high / "s");
  std::filesystem::create_symlink(high / "missing-target",
                                  high / "s" / "SKILL.md");

  auto lib = SkillLibrary::Load({{low, "builtin"}, {high, "user"}});

  ASSERT_FALSE(lib.ok());
  EXPECT_NE(lib.status().message().find("user"), std::string::npos);
  EXPECT_NE(lib.status().message().find("SKILL.md"), std::string::npos);
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
namespace agentflow::skills {
namespace {

TEST(SkillLibrary, IndexesFlatReferenceFilesOnly) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "s" / "SKILL.md", SkillMd("s", "d", ""));
  WriteFile(root / "s" / "b-ref.md", "b");
  WriteFile(root / "s" / "a-ref.md", "a");
  WriteFile(root / "s" / "scripts" / "run.sh", "echo hi");
  WriteFile(root / "s" / "nested" / "deep.md", "deep");
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  const Skill* skill = (*lib)->Find("s");
  ASSERT_NE(skill, nullptr);

  const auto& files = (*lib)->Files(*skill);
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(files[0], "a-ref.md");
  EXPECT_EQ(files[1], "b-ref.md");

  const std::string* found = (*lib)->FindFile(*skill, "a-ref.md");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(*found, "a-ref.md");
  EXPECT_EQ((*lib)->FindFile(*skill, "nope.md"), nullptr);
  EXPECT_EQ((*lib)->FindFile(*skill, "nested/deep.md"), nullptr);
  // A skill this library does not know has no files.
  Skill unknown;
  unknown.name = "not-loaded";
  EXPECT_TRUE((*lib)->Files(unknown).empty());
}

TEST(SkillLibrary, IndexCapsAtTwoHundredFilesPerSkill) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "s" / "SKILL.md", SkillMd("s", "d", ""));
  for (int i = 0; i < 205; ++i) {
    WriteFile(root / "s" / ("f" + std::to_string(1000 + i).substr(1) + ".txt"),
              "x");
  }
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  const Skill* skill = (*lib)->Find("s");
  ASSERT_NE(skill, nullptr);
  const auto& files = (*lib)->Files(*skill);
  ASSERT_EQ(files.size(), 200u);
  EXPECT_EQ(files.front(), "f000.txt");
  EXPECT_EQ(files.back(), "f199.txt");
  EXPECT_NE((*lib)->FindFile(*skill, "f199.txt"), nullptr);
  EXPECT_EQ((*lib)->FindFile(*skill, "f204.txt"), nullptr);
}

TEST(SkillLibrary, SearchRanksNameThenDescriptionThenFileHits) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "lighting" / "SKILL.md", SkillMd("lighting", "Lighting design", ""));
  WriteFile(root / "lighting-pro" / "SKILL.md", SkillMd("lighting-pro", "Advanced", ""));
  WriteFile(root / "advanced-lighting" / "SKILL.md", SkillMd("advanced-lighting", "More", ""));
  WriteFile(root / "mood-board" / "SKILL.md",
            SkillMd("mood-board", "Use lighting references for mood", ""));
  WriteFile(root / "file-only" / "SKILL.md", SkillMd("file-only", "unrelated", ""));
  WriteFile(root / "file-only" / "lighting-notes.md", "notes");
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();

  auto results = (*lib)->Search("lighting", 10);
  ASSERT_EQ(results.size(), 5u);
  EXPECT_EQ(results[0].name, "lighting");
  EXPECT_EQ(results[1].name, "lighting-pro");
  EXPECT_EQ(results[2].name, "advanced-lighting");
  EXPECT_EQ(results[3].name, "mood-board");
  EXPECT_EQ(results[4].name, "file-only");
  EXPECT_TRUE(results[0].matched.empty());
  EXPECT_TRUE(results[3].matched.empty());
  EXPECT_EQ(results[4].matched, "lighting-notes.md");

  auto limited = (*lib)->Search("lighting", 3);
  ASSERT_EQ(limited.size(), 3u);
  EXPECT_EQ(limited[2].name, "advanced-lighting");
  EXPECT_TRUE((*lib)->Search("lighting", 0).empty());
  EXPECT_TRUE((*lib)->Search("  ", 5).empty());
}

TEST(SkillLibrary, SearchBreaksTiesByName) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "aa-x" / "SKILL.md", SkillMd("aa-x", "unrelated", ""));
  WriteFile(root / "zz-x" / "SKILL.md", SkillMd("zz-x", "unrelated", ""));
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto results = (*lib)->Search("x", 10);
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].name, "aa-x");
  EXPECT_EQ(results[1].name, "zz-x");
}

TEST(SkillLibrary, SearchOnlyLooksAtVisibleSkills) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "visible" / "SKILL.md", SkillMd("visible", "lighting", ""));
  WriteFile(root / "hidden" / "SKILL.md", SkillMd("hidden", "lighting", ""));
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto results = (*lib)->Search("lighting", 10, {"visible"});
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].name, "visible");
}

TEST(SkillLibrary, SearchDescriptionNeedsEveryTerm) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "terms" / "SKILL.md", SkillMd("terms", "灯光 情绪 参考", ""));
  auto lib = SkillLibrary::Load({{root, "builtin"}});
  ASSERT_TRUE(lib.ok()) << lib.status();
  auto hit = (*lib)->Search("灯光 情绪", 5);
  ASSERT_EQ(hit.size(), 1u);
  EXPECT_EQ(hit[0].name, "terms");
  EXPECT_TRUE((*lib)->Search("灯光 舞台", 5).empty());
}

TEST(SkillLibrary, AllowedToolsAliasEmitsAMigrationWarning) {
  auto root = FreshTempDir("lib");
  WriteFile(root / "s" / "SKILL.md",
            "---\nname: s\ndescription: d\nallowed-tools: get_shot\n---\n");
  std::vector<std::string> warnings;
  auto lib = SkillLibrary::Load({{root, "builtin"}}, &warnings);
  ASSERT_TRUE(lib.ok()) << lib.status();
  const Skill* skill = (*lib)->Find("s");
  ASSERT_NE(skill, nullptr);
  EXPECT_EQ(skill->tools, (std::vector<std::string>{"get_shot"}));
  EXPECT_TRUE(skill->tools_from_allowed_tools);
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("allowed-tools"), std::string::npos);
  EXPECT_NE(warnings[0].find("alias"), std::string::npos);
  EXPECT_NE(warnings[0].find("tools"), std::string::npos);
}

}  // namespace
}  // namespace agentflow::skills
