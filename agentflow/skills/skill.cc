#include "agentflow/skills/skill.h"

#include <string>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_replace.h"
#include "absl/strings/str_split.h"
#include "yaml-cpp/yaml.h"

namespace agentflow::skills {
namespace {

constexpr size_t kMaxNameLen = 64;
constexpr size_t kMaxDescriptionLen = 1024;
constexpr size_t kMaxCompatibilityLen = 500;

absl::Status Invalid(std::string_view message) {
  return absl::InvalidArgumentError(std::string(message));
}

absl::StatusOr<std::string> Scalar(const YAML::Node& node, std::string_view key) {
  if (!node.IsScalar()) {
    return Invalid(absl::StrCat("frontmatter '", key, "' must be a string"));
  }
  return node.as<std::string>();
}

absl::Status SplitFrontmatter(const std::string& text, std::string* front,
                              std::string* body) {
  if (!absl::StartsWith(text, "---\n")) {
    return Invalid("SKILL.md must start with a '---' YAML frontmatter line");
  }
  size_t pos = 4;
  while (pos <= text.size()) {
    const size_t eol = text.find('\n', pos);
    const size_t end = eol == std::string::npos ? text.size() : eol;
    if (std::string_view(text).substr(pos, end - pos) == "---") {
      *front = text.substr(4, pos - 4);
      size_t body_start = eol == std::string::npos ? text.size() : eol + 1;
      while (body_start < text.size() && text[body_start] == '\n') ++body_start;
      *body = text.substr(body_start);
      return absl::OkStatus();
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return Invalid("SKILL.md frontmatter is not closed by a '---' line");
}

absl::Status ApplyField(const std::string& key, const YAML::Node& value,
                        Skill* skill, std::vector<std::string>* unknown) {
  if (key == "name" || key == "description" || key == "license" ||
      key == "compatibility") {
    auto parsed = Scalar(value, key);
    if (!parsed.ok()) return parsed.status();
    if (key == "name") skill->name = std::move(*parsed);
    if (key == "description") skill->description = std::move(*parsed);
    if (key == "license") skill->license = std::move(*parsed);
    if (key == "compatibility") skill->compatibility = std::move(*parsed);
    return absl::OkStatus();
  }
  if (key == "metadata") {
    if (!value.IsMap()) return Invalid("frontmatter 'metadata' must be a mapping");
    for (const auto& entry : value) {
      const std::string metadata_key = entry.first.as<std::string>();
      auto parsed = Scalar(entry.second, absl::StrCat("metadata.", metadata_key));
      if (!parsed.ok()) return parsed.status();
      skill->metadata[metadata_key] = std::move(*parsed);
    }
    return absl::OkStatus();
  }
  if (key == "allowed-tools") {
    if (value.IsScalar()) {
      for (absl::string_view tool : absl::StrSplit(
               value.as<std::string>(), absl::ByAnyChar(" \t\n"),
               absl::SkipEmpty())) {
        skill->allowed_tools.emplace_back(tool);
      }
      return absl::OkStatus();
    }
    if (value.IsSequence()) {
      for (const auto& item : value) {
        auto parsed = Scalar(item, "allowed-tools[]");
        if (!parsed.ok()) return parsed.status();
        skill->allowed_tools.push_back(std::move(*parsed));
      }
      return absl::OkStatus();
    }
    return Invalid("frontmatter 'allowed-tools' must be a string or a list");
  }
  if (unknown != nullptr) unknown->push_back(key);
  return absl::OkStatus();
}

}  // namespace

bool IsValidSkillName(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameLen || name.front() == '-' ||
      name.back() == '-' || name.find("--") != std::string_view::npos) {
    return false;
  }
  for (char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
      return false;
    }
  }
  return true;
}

absl::StatusOr<Skill> ParseSkillMd(std::string_view raw, std::string_view dir_name,
                                   std::vector<std::string>* unknown_fields_out) {
  std::string text(raw);
  if (absl::StartsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
  absl::StrReplaceAll({{"\r\n", "\n"}}, &text);

  std::string front;
  std::string body;
  if (absl::Status status = SplitFrontmatter(text, &front, &body); !status.ok()) {
    return status;
  }

  Skill skill;
  skill.body = std::move(body);
  try {
    const YAML::Node root = YAML::Load(front);
    if (!root.IsMap()) return Invalid("SKILL.md frontmatter must be a YAML mapping");
    for (const auto& field : root) {
      const std::string key = field.first.as<std::string>();
      if (absl::Status status = ApplyField(key, field.second, &skill,
                                          unknown_fields_out);
          !status.ok()) {
        return status;
      }
    }
  } catch (const YAML::Exception& error) {
    return Invalid(absl::StrCat("invalid YAML frontmatter: ", error.what()));
  }

  if (skill.name.empty()) return Invalid("frontmatter is missing required 'name'");
  if (!IsValidSkillName(skill.name)) {
    return Invalid(absl::StrCat("skill name '", skill.name,
                                "' must use 1-64 lowercase letters, digits, or single hyphens"));
  }
  if (skill.name != dir_name) {
    return Invalid(absl::StrCat("skill name '", skill.name,
                                "' must equal its directory name '", dir_name, "'"));
  }
  if (absl::StripAsciiWhitespace(skill.description).empty()) {
    return Invalid("frontmatter is missing required non-empty 'description'");
  }
  if (skill.description.size() > kMaxDescriptionLen) {
    return Invalid("frontmatter 'description' exceeds 1024 characters");
  }
  if (skill.compatibility.size() > kMaxCompatibilityLen) {
    return Invalid("frontmatter 'compatibility' exceeds 500 characters");
  }
  return skill;
}

}  // namespace agentflow::skills
