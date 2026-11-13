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
  // Tool names this skill registers when it is loaded (frontmatter `tools`).
  // Empty means the skill declares no tools.
  std::vector<std::string> tools;
  // True when `tools` was synthesized from the legacy `allowed-tools` alias;
  // SkillLibrary::Load reports a migration warning for such entries.
  bool tools_from_allowed_tools = false;
  // Parsed and retained for compatibility; v1 does not use this to select tools.
  // `allowed-tools` is permission pre-authorization, not the registration list.
  std::vector<std::string> allowed_tools;
  // Filled by SkillLibrary; empty when returned directly by ParseSkillMd.
  std::filesystem::path dir;
  // UTF-8 SKILL.md body after CRLF normalization and leading blank-line removal.
  std::string body;
  // Filled by SkillLibrary; scripts are not executed by the parser.
  bool has_scripts = false;
};

// One flat reference file inside a skill directory, e.g. `室内灯光参考.md`.
struct ReferenceFile {
  // Frontmatter `name`, or the file name with its ".md" suffix stripped.
  std::string name;
  std::string description;
  // Tools registered when the reference is loaded, exactly like skill `tools`.
  std::vector<std::string> tools;
  // Text after the frontmatter; the whole file when no frontmatter is present.
  std::string body;
};

// Names use 1-64 lowercase ASCII letters, digits, and single hyphens.
bool IsValidSkillName(std::string_view name);

// Parses SKILL.md frontmatter and body. Accepts UTF-8 BOM and CRLF. Unknown
// frontmatter fields are ignored and appended to unknown_fields_out in file order.
// `tools` must be a YAML list of unique non-empty strings (at most 64); when
// `tools` is absent and `allowed-tools` is present, allowed-tools is treated as
// the migration alias for `tools` and `tools_from_allowed_tools` is set.
absl::StatusOr<Skill> ParseSkillMd(
    std::string_view text, std::string_view dir_name,
    std::vector<std::string>* unknown_fields_out = nullptr);

// Parses an optional YAML frontmatter block from a reference file. Only `name`,
// `description`, and `tools` are recognized; other fields are ignored. A file
// without a closed frontmatter block is returned verbatim as the body.
absl::StatusOr<ReferenceFile> ParseReferenceMd(std::string_view text,
                                               std::string_view filename);

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_H_
