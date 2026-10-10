// Loads skills from ordered roots. Later roots have higher priority.
#ifndef AGENTFLOW_SKILLS_SKILL_LIBRARY_H_
#define AGENTFLOW_SKILLS_SKILL_LIBRARY_H_

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

class SkillLibrary {
 public:
  static absl::StatusOr<std::shared_ptr<const SkillLibrary>> Load(
      const std::vector<SkillRoot>& roots,
      std::vector<std::string>* warnings = nullptr);

  const Skill* Find(std::string_view name) const;
  std::vector<const Skill*> All() const;
  absl::StatusOr<std::vector<const Skill*>> Select(
      const std::vector<std::string>& names) const;

 private:
  std::map<std::string, Skill, std::less<>> skills_;
};

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_LIBRARY_H_
