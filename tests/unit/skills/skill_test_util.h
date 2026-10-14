#ifndef AGENTFLOW_TESTS_UNIT_SKILLS_SKILL_TEST_UTIL_H_
#define AGENTFLOW_TESTS_UNIT_SKILLS_SKILL_TEST_UTIL_H_

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

namespace agentflow::skills::testing_util {

inline std::filesystem::path FreshTempDir(std::string_view tag) {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  auto dir = std::filesystem::path(::testing::TempDir()) /
             (std::string(tag) + "_" + info->test_suite_name() + "_" + info->name());
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

inline void WriteFile(const std::filesystem::path& p, std::string_view content) {
  std::filesystem::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary);
  out << content;
}

inline std::string SkillMd(std::string_view name, std::string_view description,
                           std::string_view body) {
  return "---\nname: " + std::string(name) + "\ndescription: " +
         std::string(description) + "\n---\n" + std::string(body);
}

}  // namespace agentflow::skills::testing_util
#endif  // AGENTFLOW_TESTS_UNIT_SKILLS_SKILL_TEST_UTIL_H_
