#ifndef AGENTFLOW_WORKFLOW_JSON_PATH_H_
#define AGENTFLOW_WORKFLOW_JSON_PATH_H_

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "absl/status/statusor.h"
#include <nlohmann/json.hpp>

namespace agentflow::workflow {

// JSONPath subset: $.field[N].field. No filters, no wildcards, no recursive descent.
class JsonPath {
 public:
  [[nodiscard]] static absl::StatusOr<JsonPath> Parse(std::string_view expr);
  [[nodiscard]] std::optional<nlohmann::ordered_json> Resolve(
      const nlohmann::ordered_json& root) const;
 private:
  using Segment = std::variant<std::string, int>;
  std::vector<Segment> segments_;
};

}  // namespace agentflow::workflow

#endif  // AGENTFLOW_WORKFLOW_JSON_PATH_H_
