// Loads skills from ordered roots. Later roots have higher priority.
#ifndef AGENTFLOW_SKILLS_SKILL_LIBRARY_H_
#define AGENTFLOW_SKILLS_SKILL_LIBRARY_H_

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/statusor.h"
#include "agentflow/skills/skill.h"

namespace agentflow::skills {

struct SkillRoot {
  std::filesystem::path dir;
  std::string label;
};

// One hit for find_skill. `matched` is the reference file name for a file hit
// and empty when the skill name or description matched.
struct SkillSearchResult {
  std::string name;
  std::string description;
  std::string matched;
};

class SkillLibrary {
 public:
  static absl::StatusOr<std::shared_ptr<const SkillLibrary>> Load(
      const std::vector<SkillRoot>& roots,
      std::vector<std::string>* warnings = nullptr);

  const Skill* Find(std::string_view name) const;
  std::vector<const Skill*> All() const;
  absl::StatusOr<std::vector<const Skill*>> Select(
      const std::vector<std::string>& names) const;

  // Flat files indexed for `skill` when the library was loaded: SKILL.md is
  // excluded, subdirectories are skipped, at most 200 files per skill, sorted
  // by name. Returns an empty vector for a skill this library does not know.
  const std::vector<std::string>& Files(const Skill& skill) const;
  // Returns the indexed file whose name equals `name`, or nullptr.
  const std::string* FindFile(const Skill& skill, std::string_view name) const;

  // Ranks skills for `query`: exact name match > name prefix > name contains >
  // description hit > file name hit; ties break by skill name, then file name.
  // Name matching uses the lowercased query; description and file matching
  // require every whitespace-separated query term. An empty `visible_skills`
  // searches every loaded skill.
  std::vector<SkillSearchResult> Search(
      std::string_view query, std::size_t limit,
      const std::vector<std::string>& visible_skills = {}) const;

 private:
  std::map<std::string, Skill, std::less<>> skills_;
  std::map<std::string, std::vector<std::string>, std::less<>> files_;
};

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_LIBRARY_H_
