// Internal helper shared by the skills sources. Not listed in the target's hdrs.
#ifndef AGENTFLOW_SKILLS_SKILL_UTF8_H_
#define AGENTFLOW_SKILLS_SKILL_UTF8_H_

#include <string_view>

namespace agentflow::skills {

// Strict UTF-8: rejects overlong forms, surrogates, and code points above U+10FFFF.
bool IsValidUtf8(std::string_view text);

}  // namespace agentflow::skills
#endif  // AGENTFLOW_SKILLS_SKILL_UTF8_H_
