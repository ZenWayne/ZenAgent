// Read-only skill tools and prompt catalog. Hosts register these per agent
// using the agent's visible skill subset.
#ifndef AGENTFLOW_SKILLS_SKILL_TOOLS_H_
#define AGENTFLOW_SKILLS_SKILL_TOOLS_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agentflow/skills/skill_library.h"
#include "agentflow/tools/tool.h"

namespace agentflow::skills {

inline constexpr std::string_view kLoadSkillToolName = "load_skill";
inline constexpr std::string_view kReadSkillFileToolName = "read_skill_file";
inline constexpr std::size_t kMaxSkillFileBytes = 262144;

// Returns no tools for an empty subset. Invalid names or duplicate names
// cause std::invalid_argument rather than exposing a broken schema.
std::vector<std::shared_ptr<agentflow::Tool>> MakeSkillTools(
    std::shared_ptr<const SkillLibrary> library,
    std::vector<std::string> visible_names);

// Host appends this section after rendering its system prompt.
std::string RenderSkillCatalog(const std::vector<const Skill*>& skills);

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_TOOLS_H_
