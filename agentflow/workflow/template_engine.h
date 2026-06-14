#ifndef AGENTFLOW_WORKFLOW_TEMPLATE_ENGINE_H_
#define AGENTFLOW_WORKFLOW_TEMPLATE_ENGINE_H_

#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <absl/status/statusor.h>
#include <nlohmann/json.hpp>

#include "agentflow/workflow/eval_context.h"

namespace agentflow::workflow {

// Pure-substitution template, no expressions.
// Parse() validates brace balance + known path heads (state|parent|tool|workflow|now).
// Evaluate() returns either a JSON value (single-substitution mode, type preserved)
// or a string (interpolation mode).
class TemplateString {
 public:
  [[nodiscard]] static absl::StatusOr<TemplateString> Parse(std::string_view expr);
  std::string_view source() const { return source_; }
  // Materialize the list of substitution path heads/parts on demand from the
  // parsed segments. Phase 3's loader uses this to validate that referenced
  // {{state.X}} paths exist in the agent's state schema.
  std::vector<std::vector<std::string>> paths() const;
  bool single_substitution() const { return single_substitution_; }
  [[nodiscard]] nlohmann::ordered_json Evaluate(const EvalContext& ctx) const;

 private:
  struct LiteralSeg { std::string text; };
  struct PathSeg    { std::vector<std::string> parts; };
  using Segment = std::variant<LiteralSeg, PathSeg>;
  std::string source_;
  std::vector<Segment> segs_;
  bool single_substitution_ = false;
};

}  // namespace agentflow::workflow

#endif  // AGENTFLOW_WORKFLOW_TEMPLATE_ENGINE_H_
