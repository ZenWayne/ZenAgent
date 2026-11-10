#include "agentflow/skills/skill.h"
#include "agentflow/skills/skill_utf8.h"

#include <algorithm>
#include <cstddef>
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
constexpr size_t kMaxTools = 64;

absl::Status Invalid(std::string_view message) {
  return absl::InvalidArgumentError(std::string(message));
}

absl::StatusOr<std::string> Scalar(const YAML::Node& node, std::string_view key) {
  if (!node.IsScalar()) {
    return Invalid(absl::StrCat("frontmatter '", key, "' must be a string"));
  }
  return node.as<std::string>();
}

absl::StatusOr<size_t> Utf8CharacterCount(std::string_view value,
                                         std::string_view value_name) {
  if (!IsValidUtf8(value)) {
    return Invalid(absl::StrCat(value_name, " must contain valid UTF-8"));
  }
  // Each code point starts with exactly one non-continuation byte.
  size_t count = 0;
  for (char c : value) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
  }
  return count;
}

// Splits an optional frontmatter block. Returns false when the text does not
// start with a closed '---' block; the caller then treats it as plain text.
bool SplitOptionalFrontmatter(const std::string& text, std::string* front,
                              std::string* body) {
  if (!absl::StartsWith(text, "---\n")) return false;
  size_t pos = 4;
  while (pos <= text.size()) {
    const size_t eol = text.find('\n', pos);
    const size_t end = eol == std::string::npos ? text.size() : eol;
    if (std::string_view(text).substr(pos, end - pos) == "---") {
      *front = text.substr(4, pos - 4);
      size_t body_start = eol == std::string::npos ? text.size() : eol + 1;
      while (body_start < text.size() && text[body_start] == '\n') ++body_start;
      *body = text.substr(body_start);
      return true;
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return false;
}

absl::Status SplitFrontmatter(const std::string& text, std::string* front,
                              std::string* body) {
  if (!absl::StartsWith(text, "---\n")) {
    return Invalid("SKILL.md must start with a '---' YAML frontmatter line");
  }
  if (!SplitOptionalFrontmatter(text, front, body)) {
    return Invalid("SKILL.md frontmatter is not closed by a '---' line");
  }
  return absl::OkStatus();
}

// A string list of tool names: entries must be non-empty strings, duplicates
// are dropped (first occurrence wins), and at most 64 distinct names survive.
absl::StatusOr<std::vector<std::string>> ParseToolList(const YAML::Node& value,
                                                       std::string_view key) {
  if (!value.IsSequence()) {
    return Invalid(absl::StrCat("frontmatter '", key,
                                "' must be a list of tool names"));
  }
  std::vector<std::string> tools;
  for (const auto& item : value) {
    auto parsed = Scalar(item, absl::StrCat(key, "[]"));
    if (!parsed.ok()) return parsed.status();
    if (parsed->empty()) {
      return Invalid(absl::StrCat("frontmatter '", key,
                                  "' contains an empty tool name"));
    }
    if (std::find(tools.begin(), tools.end(), *parsed) != tools.end()) continue;
    if (tools.size() >= kMaxTools) {
      return Invalid(absl::StrCat("frontmatter '", key,
                                  "' exceeds 64 distinct tool names"));
    }
    tools.push_back(std::move(*parsed));
  }
  return tools;
}

absl::Status ApplyField(const std::string& key, const YAML::Node& value,
                        Skill* skill, bool* has_tools_field,
                        std::vector<std::string>* unknown) {
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
  if (key == "tools") {
    auto parsed = ParseToolList(value, "tools");
    if (!parsed.ok()) return parsed.status();
    skill->tools = std::move(*parsed);
    if (has_tools_field != nullptr) *has_tools_field = true;
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
  auto body_length = Utf8CharacterCount(body, "SKILL.md body");
  if (!body_length.ok()) return body_length.status();

  Skill skill;
  skill.body = std::move(body);
  bool has_tools_field = false;
  try {
    const YAML::Node root = YAML::Load(front);
    if (!root.IsMap()) return Invalid("SKILL.md frontmatter must be a YAML mapping");
    for (const auto& field : root) {
      const std::string key = field.first.as<std::string>();
      if (absl::Status status = ApplyField(key, field.second, &skill,
                                          &has_tools_field, unknown_fields_out);
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
  auto description_length =
      Utf8CharacterCount(skill.description, "frontmatter 'description'");
  if (!description_length.ok()) return description_length.status();
  if (*description_length > kMaxDescriptionLen) {
    return Invalid("frontmatter 'description' exceeds 1024 characters");
  }
  auto compatibility_length =
      Utf8CharacterCount(skill.compatibility, "frontmatter 'compatibility'");
  if (!compatibility_length.ok()) return compatibility_length.status();
  if (*compatibility_length > kMaxCompatibilityLen) {
    return Invalid("frontmatter 'compatibility' exceeds 500 characters");
  }
  // Migration: `allowed-tools` stays permission pre-authorization, but a skill
  // that only wrote `allowed-tools` still gets those names as its `tools`.
  if (!has_tools_field && !skill.allowed_tools.empty()) {
    skill.tools = skill.allowed_tools;
    skill.tools_from_allowed_tools = true;
  }
  return skill;
}

absl::StatusOr<ReferenceFile> ParseReferenceMd(std::string_view raw,
                                               std::string_view filename) {
  std::string text(raw);
  if (absl::StartsWith(text, "\xEF\xBB\xBF")) text.erase(0, 3);
  absl::StrReplaceAll({{"\r\n", "\n"}}, &text);

  ReferenceFile ref;
  std::string stem = std::filesystem::path(std::string(filename)).filename().string();
  if (absl::EndsWith(stem, ".md")) stem.resize(stem.size() - 3);
  if (stem.empty()) return Invalid("reference file name must not be empty");
  ref.name = std::move(stem);

  std::string front;
  std::string body;
  if (!SplitOptionalFrontmatter(text, &front, &body)) {
    ref.body = std::move(text);
    return ref;
  }
  if (!front.empty()) {
    try {
      const YAML::Node root = YAML::Load(front);
      if (!root.IsMap()) {
        return Invalid(absl::StrCat("reference '", filename,
                                    "' frontmatter must be a YAML mapping"));
      }
      for (const auto& field : root) {
        const std::string key = field.first.as<std::string>();
        if (key == "name") {
          auto parsed = Scalar(field.second, "name");
          if (!parsed.ok()) return parsed.status();
          if (parsed->empty()) {
            return Invalid("reference frontmatter 'name' must not be empty");
          }
          ref.name = std::move(*parsed);
        } else if (key == "description") {
          auto parsed = Scalar(field.second, "description");
          if (!parsed.ok()) return parsed.status();
          ref.description = std::move(*parsed);
        } else if (key == "tools") {
          auto parsed = ParseToolList(field.second, "tools");
          if (!parsed.ok()) return parsed.status();
          ref.tools = std::move(*parsed);
        }
        // References are data, not a manifest: unknown fields are ignored.
      }
    } catch (const YAML::Exception& error) {
      return Invalid(absl::StrCat("invalid reference frontmatter: ", error.what()));
    }
  }
  ref.body = std::move(body);
  return ref;
}

}  // namespace agentflow::skills
