// Skill tools and prompt catalog. Hosts register these per session using the
// agent's visible skill subset and a load hook that owns tool registration.
#ifndef AGENTFLOW_SKILLS_SKILL_TOOLS_H_
#define AGENTFLOW_SKILLS_SKILL_TOOLS_H_

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "agentflow/skills/skill_library.h"
#include "agentflow/tools/tool.h"

namespace agentflow::skills {

inline constexpr std::string_view kFindSkillToolName = "find_skill";
inline constexpr std::string_view kLoadSkillToolName = "load_skill";
inline constexpr std::string_view kReadSkillFileToolName = "read_skill_file";
inline constexpr std::size_t kMaxSkillFileBytes = 262144;

// One successful load_skill resolution. The hook receives it AFTER the body has
// been composed and registers `tool_names` atomically; returning an error makes
// the whole load fail (no body, no state change).
struct SkillLoadResult {
  // Exact tool result text, including the final "Registered tools: ..." line
  // when the load declares tools.
  std::string text;
  // Declared tools for this load; the hook must register exactly these.
  std::vector<std::string> tool_names;
  // Owning skill. Set for both skill and reference-file loads.
  std::string loaded_skill;
  // Relative reference file path; empty for a top-level skill load.
  std::string loaded_file;
  // True when a reference file was loaded instead of SKILL.md.
  bool is_file = false;
};

// Called on every successful resolution, even when no tool is declared.
// Without a hook the tools behave like the read-only v1 pair.
using SkillLoadHook = std::function<absl::Status(const SkillLoadResult&)>;

// Builds a hook failure that load_skill reports with `code` (one of the skill
// tool error codes, e.g. "skill_tool_ungraded") instead of the generic
// "tool_registration_failed". The message is returned to the model verbatim.
absl::Status SkillToolError(std::string_view code, std::string_view message);

// Returns no tools for an empty subset (even without a library). A nonempty
// subset requires a library and valid, unique names; errors return a status.
absl::StatusOr<std::vector<std::shared_ptr<agentflow::Tool>>> MakeSkillTools(
    std::shared_ptr<const SkillLibrary> library,
    std::vector<std::string> visible_names,
    SkillLoadHook on_load = {});

// Host appends this section after rendering its system prompt.
std::string RenderSkillCatalog(const std::vector<const Skill*>& skills);

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_TOOLS_H_
