// Interface only. A future local host may implement script execution, after
// passing its confirmation gate for every run. Server builds must never link
// an implementation or expose one as a tool. script_rel_path must resolve
// inside skill.dir / "scripts" under read_skill_file's escape rules.
#ifndef AGENTFLOW_SKILLS_SKILL_SCRIPT_RUNNER_H_
#define AGENTFLOW_SKILLS_SKILL_SCRIPT_RUNNER_H_

#include <string>
#include <string_view>

#include <asio/awaitable.hpp>

#include "absl/status/statusor.h"
#include "agentflow/core/cancel.h"
#include "agentflow/skills/skill.h"

namespace agentflow::skills {

class SkillScriptRunner {
 public:
  virtual ~SkillScriptRunner() = default;
  virtual asio::awaitable<absl::StatusOr<std::string>> Run(
      const Skill& skill, std::string_view script_rel_path,
      std::string_view args_json, const CancelToken& cancel) = 0;
};

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_SCRIPT_RUNNER_H_
