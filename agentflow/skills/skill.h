#ifndef AGENTFLOW_SKILLS_SKILL_H_
#define AGENTFLOW_SKILLS_SKILL_H_

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"

namespace agentflow::skills {

struct Skill {
  std::string name;
  std::string description;
  std::string license;
  std::string compatibility;
  std::map<std::string, std::string> metadata;
  // Parsed and retained for compatibility; v1 does not use this to select tools.
  std::vector<std::string> allowed_tools;
  // Filled by SkillLibrary; empty when returned directly by ParseSkillMd.
  std::filesystem::path dir;
  // SKILL.md body, preserved verbatim after line-ending normalization.
  std::string body;
  // Filled by SkillLibrary; scripts are not executed by the parser.
  bool has_scripts = false;
};

// Names use 1-64 lowercase ASCII letters, digits, and single hyphens.
bool IsValidSkillName(std::string_view name);

// Parses SKILL.md frontmatter and body. Accepts UTF-8 BOM and CRLF. Unknown
// frontmatter fields are ignored and appended to unknown_fields_out in file order.
absl::StatusOr<Skill> ParseSkillMd(
    std::string_view text, std::string_view dir_name,
    std::vector<std::string>* unknown_fields_out = nullptr);

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_H_
