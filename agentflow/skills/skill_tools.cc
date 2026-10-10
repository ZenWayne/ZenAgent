#include "agentflow/skills/skill_tools.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "agentflow/tools/native_fn_tool.h"

namespace agentflow::skills {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string Error(std::string_view code, std::string_view message) {
  return json{{"error", std::string(code)}, {"message", std::string(message)}}.dump();
}

bool IsValidUtf8(std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }
    size_t trailing = 0;
    unsigned int point = 0;
    unsigned int minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
      trailing = 1; point = lead & 0x1F; minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      trailing = 2; point = lead & 0x0F; minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      trailing = 3; point = lead & 0x07; minimum = 0x10000;
    } else {
      return false;
    }
    if (trailing > text.size() - i - 1) return false;
    for (size_t k = 1; k <= trailing; ++k) {
      const auto byte = static_cast<unsigned char>(text[i + k]);
      if ((byte & 0xC0) != 0x80) return false;
      point = (point << 6) | (byte & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF ||
        (point >= 0xD800 && point <= 0xDFFF)) return false;
    i += trailing + 1;
  }
  return true;
}

const Skill* VisibleSkill(const json& args, const char* key,
                          const SkillLibrary& library,
                          const std::vector<std::string>& visible,
                          std::string* error) {
  if (!args.is_object() || !args.contains(key) || !args[key].is_string()) {
    *error = Error("bad_args", absl::StrCat("'", key, "' must be a string"));
    return nullptr;
  }
  const std::string name = args[key].get<std::string>();
  const Skill* skill =
      std::find(visible.begin(), visible.end(), name) == visible.end()
          ? nullptr : library.Find(name);
  if (skill == nullptr) {
    *error = Error("unknown_skill", absl::StrCat(
        "unknown skill '", name, "'; available: ", absl::StrJoin(visible, ", ")));
  }
  return skill;
}

std::string LoadSkill(const SkillLibrary& library,
                      const std::vector<std::string>& visible,
                      std::string_view args_json) {
  const json args = json::parse(args_json, nullptr, false);
  std::string error;
  const Skill* skill = VisibleSkill(args, "name", library, visible, &error);
  if (skill == nullptr) return error;
  return absl::StrCat("Skill directory: ", skill->name,
      "/ — use read_skill_file(skill, path) for files referenced below.\n\n",
      skill->body);
}

bool IsContained(const fs::path& base, const fs::path& target) {
  auto base_it = base.begin();
  auto target_it = target.begin();
  for (; base_it != base.end(); ++base_it, ++target_it) {
    if (target_it == target.end() || *base_it != *target_it) return false;
  }
  return target_it != target.end();  // A directory is not a readable file.
}

std::string ReadSkillFile(const SkillLibrary& library,
                          const std::vector<std::string>& visible,
                          std::string_view args_json) {
  const json args = json::parse(args_json, nullptr, false);
  std::string error;
  const Skill* skill = VisibleSkill(args, "skill", library, visible, &error);
  if (skill == nullptr) return error;
  if (!args.contains("path") || !args["path"].is_string()) {
    return Error("bad_args", "'path' must be a string");
  }
  const std::string rel_str = args["path"].get<std::string>();
  const fs::path rel(rel_str);
  if (rel_str.empty() || rel_str.find('\0') != std::string::npos ||
      rel.is_absolute() || rel.has_root_name() ||
      std::find(rel.begin(), rel.end(), fs::path("..")) != rel.end()) {
    return Error("path_not_allowed", "path must stay within the skill directory");
  }

  std::error_code ec;
  const fs::path target = fs::weakly_canonical(skill->dir / rel, ec);
  if (ec) return Error("not_found", absl::StrCat("cannot resolve '", rel_str, "'"));
  if (!IsContained(skill->dir, target)) {
    return Error("path_not_allowed", "path escapes the skill directory");
  }
  const fs::file_status status = fs::status(target, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      status.type() == fs::file_type::not_found) {
    return Error("not_found", absl::StrCat("no such file '", rel_str, "'"));
  }
  if (ec || !fs::is_regular_file(status)) {
    return Error("not_a_file", absl::StrCat("'", rel_str, "' is not a regular file"));
  }
  const auto size = fs::file_size(target, ec);
  if (ec) return Error("not_found", absl::StrCat("cannot read '", rel_str, "'"));
  if (size > kMaxSkillFileBytes) {
    return Error("too_large", absl::StrCat("'", rel_str, "' exceeds ",
                                            kMaxSkillFileBytes, " bytes"));
  }

  std::ifstream input(target, std::ios::binary);
  if (!input) return Error("not_found", absl::StrCat("cannot read '", rel_str, "'"));
  // The second cap protects against growth between file_size and the read.
  std::string content(kMaxSkillFileBytes + 1, '\0');
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  const auto bytes_read = input.gcount();
  if (input.bad()) return Error("not_found", absl::StrCat("cannot read '", rel_str, "'"));
  if (bytes_read > static_cast<std::streamsize>(kMaxSkillFileBytes)) {
    return Error("too_large", absl::StrCat("'", rel_str, "' exceeds ",
                                            kMaxSkillFileBytes, " bytes"));
  }
  content.resize(static_cast<size_t>(bytes_read));
  if (!IsValidUtf8(content)) {
    return Error("not_utf8", absl::StrCat("'", rel_str, "' is not UTF-8 text"));
  }
  return content;
}

}  // namespace

std::vector<std::shared_ptr<agentflow::Tool>> MakeSkillTools(
    std::shared_ptr<const SkillLibrary> library,
    std::vector<std::string> visible_names) {
  if (visible_names.empty()) return {};
  if (library == nullptr) throw std::invalid_argument("skill library is null");
  const auto selected = library->Select(visible_names);
  if (!selected.ok()) throw std::invalid_argument(std::string(selected.status().message()));

  const json name_enum = visible_names;
  const json load_schema = {
      {"type", "object"},
      {"properties", {{"name", {{"type", "string"}, {"enum", name_enum},
                                {"description", "Skill to load."}}}}},
      {"required", json::array({"name"})},
      {"additionalProperties", false}};
  const json read_schema = {
      {"type", "object"},
      {"properties", {{"skill", {{"type", "string"}, {"enum", name_enum},
                                 {"description", "Skill that owns the file."}}},
                       {"path", {{"type", "string"},
                                 {"description", "File path relative to the skill directory."}}}}},
      {"required", json::array({"skill", "path"})},
      {"additionalProperties", false}};

  auto visible = std::make_shared<const std::vector<std::string>>(std::move(visible_names));
  std::vector<std::shared_ptr<agentflow::Tool>> tools;
  tools.push_back(std::make_shared<agentflow::NativeFnTool>(
      agentflow::ToolSchema{std::string(kLoadSkillToolName),
                            "Load a skill's full instructions by name.", load_schema.dump()},
      [library, visible](std::string_view args, std::string_view,
                         const CancelToken&) -> asio::awaitable<std::string> {
        co_return LoadSkill(*library, *visible, args);
      }));
  tools.push_back(std::make_shared<agentflow::NativeFnTool>(
      agentflow::ToolSchema{std::string(kReadSkillFileToolName),
                            "Read a skill's text file, including references and scripts. "
                            "Scripts are never executed.", read_schema.dump()},
      [library, visible](std::string_view args, std::string_view,
                         const CancelToken&) -> asio::awaitable<std::string> {
        co_return ReadSkillFile(*library, *visible, args);
      }));
  return tools;
}

std::string RenderSkillCatalog(const std::vector<const Skill*>& skills) {
  if (skills.empty()) return "";
  std::string out =
      "## 技能\n"
      "以下技能按需加载：判断当前任务用得上时，先调用 load_skill(name) 读完全文再动手。\n";
  for (const Skill* skill : skills) {
    std::vector<absl::string_view> words = absl::StrSplit(
        skill->description, absl::ByAnyChar(" \t\r\n"), absl::SkipEmpty());
    absl::StrAppend(&out, "- ", skill->name, ": ", absl::StrJoin(words, " "), "\n");
  }
  return out;
}

}  // namespace agentflow::skills
