#include "agentflow/workflow/json_path.h"

#include <cctype>
#include <cstdlib>
#include <utility>

#include "absl/strings/str_cat.h"

namespace agentflow::workflow {

absl::StatusOr<JsonPath> JsonPath::Parse(std::string_view expr) {
  if (expr.empty() || expr[0] != '$') {
    return absl::InvalidArgumentError("path must start with '$'");
  }
  JsonPath out;
  size_t i = 1;
  while (i < expr.size()) {
    if (expr[i] == '.') {
      size_t j = i + 1;
      while (j < expr.size() && expr[j] != '.' && expr[j] != '[') ++j;
      if (j == i + 1) return absl::InvalidArgumentError("empty field name after '.'");
      out.segments_.emplace_back(std::string(expr.substr(i + 1, j - i - 1)));
      i = j;
    } else if (expr[i] == '[') {
      size_t close = expr.find(']', i + 1);
      if (close == std::string_view::npos) return absl::InvalidArgumentError("unbalanced '['");
      std::string idx_s(expr.substr(i + 1, close - i - 1));
      for (char c : idx_s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
          return absl::InvalidArgumentError(absl::StrCat("non-integer index '", idx_s, "'"));
        }
      }
      out.segments_.emplace_back(std::atoi(idx_s.c_str()));
      i = close + 1;
    } else {
      return absl::InvalidArgumentError(absl::StrCat("unexpected char at position ", i));
    }
  }
  return out;
}

std::optional<nlohmann::ordered_json> JsonPath::Resolve(const nlohmann::ordered_json& root) const {
  const nlohmann::ordered_json* cur = &root;
  for (const auto& seg : segments_) {
    if (std::holds_alternative<std::string>(seg)) {
      const auto& key = std::get<std::string>(seg);
      if (!cur->is_object() || !cur->contains(key)) return std::nullopt;
      cur = &cur->at(key);
    } else {
      int idx = std::get<int>(seg);
      if (!cur->is_array() || idx < 0 || static_cast<size_t>(idx) >= cur->size()) {
        return std::nullopt;
      }
      cur = &cur->at(static_cast<size_t>(idx));
    }
  }
  return *cur;
}

}  // namespace agentflow::workflow
