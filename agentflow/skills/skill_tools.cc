#include "agentflow/skills/skill_tools.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "agentflow/skills/skill_utf8.h"
#include "agentflow/tools/native_fn_tool.h"

namespace agentflow::skills {
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr int kDefaultFindLimit = 5;
constexpr int kMaxFindLimit = 50;

// Every error code a load_skill result may carry. Kept in one place so status
// messages from the hook and from path resolution decode the same way.
const std::set<std::string>& KnownErrorCodes() {
  static const std::set<std::string> kCodes = {
      "bad_args",
      "unknown_skill",
      "ambiguous_skill_file",
      "path_not_allowed",
      "not_found",
      "not_a_file",
      "too_large",
      "not_utf8",
      "bad_reference",
      "skill_tool_ungraded",
      "skill_tool_blocked",
      "tool_registration_unsupported",
      "tool_registration_failed",
  };
  return kCodes;
}

std::string ErrorJson(std::string_view code, std::string_view message) {
  return json{{"error", std::string(code)}, {"message", std::string(message)}}.dump();
}

// Internal encoding: "<code>: <message>". Statuses cross helper boundaries and
// the tool result is rebuilt from them, so the code never gets lost.
absl::Status LoadError(std::string_view code, std::string_view message) {
  return absl::InvalidArgumentError(absl::StrCat(code, ": ", message));
}

std::string StatusToErrorJson(const absl::Status& status) {
  const std::string message(status.message());
  const std::size_t separator = message.find(": ");
  if (separator != std::string::npos) {
    const std::string code = message.substr(0, separator);
    if (KnownErrorCodes().contains(code)) {
      return ErrorJson(code, message.substr(separator + 2));
    }
  }
  return ErrorJson("tool_registration_failed", message);
}

std::string AvailableList(const std::vector<std::string>& visible) {
  return absl::StrJoin(visible, ", ");
}

std::string UnknownSkillMessage(const std::vector<std::string>& visible,
                                std::string_view name) {
  return absl::StrCat("unknown skill '", name, "'; available: ",
                      AvailableList(visible));
}

const Skill* VisibleSkill(const SkillLibrary& library,
                          const std::vector<std::string>& visible,
                          std::string_view name) {
  if (std::find(visible.begin(), visible.end(), name) == visible.end()) {
    return nullptr;
  }
  return library.Find(name);
}

bool IsContained(const fs::path& base, const fs::path& target) {
  auto base_it = base.begin();
  auto target_it = target.begin();
  for (; base_it != base.end(); ++base_it, ++target_it) {
    if (target_it == target.end() || *base_it != *target_it) return false;
  }
  return target_it != target.end();  // A directory is not a readable file.
}

// Resolves a relative path inside the skill directory and reads it with the
// read_skill_file limits: no absolute paths, no "..", no symlink escape, only
// regular files, at most 256 KiB, strict UTF-8.
absl::StatusOr<std::string> ReadSkillFileContent(const Skill& skill,
                                                 std::string_view rel_str) {
  const fs::path rel{std::string(rel_str)};
  if (rel_str.empty() || rel_str.find('\0') != std::string_view::npos ||
      rel.is_absolute() || rel.has_root_name() ||
      std::find(rel.begin(), rel.end(), fs::path("..")) != rel.end()) {
    return LoadError("path_not_allowed",
                     "path must stay within the skill directory");
  }

  std::error_code ec;
  const fs::path target = fs::weakly_canonical(skill.dir / rel, ec);
  if (ec) {
    return LoadError("not_found", absl::StrCat("cannot resolve '", rel_str, "'"));
  }
  if (!IsContained(skill.dir, target)) {
    return LoadError("path_not_allowed", "path escapes the skill directory");
  }
  const fs::file_status status = fs::status(target, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      status.type() == fs::file_type::not_found) {
    return LoadError("not_found", absl::StrCat("no such file '", rel_str, "'"));
  }
  if (ec || !fs::is_regular_file(status)) {
    return LoadError("not_a_file",
                     absl::StrCat("'", rel_str, "' is not a regular file"));
  }
  const auto size = fs::file_size(target, ec);
  if (ec) {
    return LoadError("not_found", absl::StrCat("cannot read '", rel_str, "'"));
  }
  if (size > kMaxSkillFileBytes) {
    return LoadError("too_large", absl::StrCat("'", rel_str, "' exceeds ",
                                              kMaxSkillFileBytes, " bytes"));
  }

  std::ifstream input(target, std::ios::binary);
  if (!input) {
    return LoadError("not_found", absl::StrCat("cannot read '", rel_str, "'"));
  }
  // The second cap protects against growth between file_size and the read.
  std::string content(kMaxSkillFileBytes + 1, '\0');
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  const auto bytes_read = input.gcount();
  if (input.bad()) {
    return LoadError("not_found", absl::StrCat("cannot read '", rel_str, "'"));
  }
  if (bytes_read > static_cast<std::streamsize>(kMaxSkillFileBytes)) {
    return LoadError("too_large", absl::StrCat("'", rel_str, "' exceeds ",
                                              kMaxSkillFileBytes, " bytes"));
  }
  content.resize(static_cast<size_t>(bytes_read));
  if (!IsValidUtf8(content)) {
    return LoadError("not_utf8",
                     absl::StrCat("'", rel_str, "' is not UTF-8 text"));
  }
  return content;
}

struct LoadOutcome {
  std::string content;
  std::vector<std::string> tools;
  std::string skill;
  std::string file;
  bool is_file = false;
};

absl::StatusOr<LoadOutcome> SkillOutcome(const Skill& skill) {
  LoadOutcome outcome;
  outcome.content = skill.body;
  outcome.tools = skill.tools;
  outcome.skill = skill.name;
  return outcome;
}

absl::StatusOr<LoadOutcome> FileOutcome(const Skill& skill,
                                        std::string_view rel) {
  auto content = ReadSkillFileContent(skill, rel);
  if (!content.ok()) return content.status();
  auto reference = ParseReferenceMd(
      *content, fs::path(std::string(rel)).filename().string());
  if (!reference.ok()) {
    return LoadError("bad_reference",
                     absl::StrCat(rel, ": ", reference.status().message()));
  }
  LoadOutcome outcome;
  outcome.content = std::move(reference->body);
  outcome.tools = std::move(reference->tools);
  outcome.skill = skill.name;
  outcome.file = std::string(rel);
  outcome.is_file = true;
  return outcome;
}

// Resolution order (see the plan, §3.3): top-level skill, explicit
// skill+path, a .md file in the current skill, a .md file unique across the
// visible skills, then unknown_skill.
absl::StatusOr<LoadOutcome> ResolveLoad(
    const SkillLibrary& library, const std::vector<std::string>& visible,
    std::string_view current_skill, const std::string& name, bool has_path,
    const std::string& path) {
  if (has_path) {
    const Skill* skill = VisibleSkill(library, visible, name);
    if (skill == nullptr) {
      return LoadError("unknown_skill", UnknownSkillMessage(visible, name));
    }
    return FileOutcome(*skill, path);
  }
  if (const Skill* skill = VisibleSkill(library, visible, name)) {
    return SkillOutcome(*skill);
  }
  if (absl::EndsWith(name, ".md")) {
    if (!current_skill.empty()) {
      const Skill* current = VisibleSkill(library, visible, current_skill);
      if (current != nullptr && library.FindFile(*current, name) != nullptr) {
        return FileOutcome(*current, name);
      }
    }
    std::vector<const Skill*> matches;
    for (const std::string& candidate : visible) {
      const Skill* skill = library.Find(candidate);
      if (skill != nullptr && library.FindFile(*skill, name) != nullptr) {
        matches.push_back(skill);
      }
    }
    if (matches.size() == 1) return FileOutcome(*matches[0], name);
    if (matches.size() > 1) {
      std::vector<std::string> owners;
      for (const Skill* skill : matches) owners.push_back(skill->name);
      return LoadError(
          "ambiguous_skill_file",
          absl::StrCat("file '", name, "' exists in several skills: ",
                       absl::StrJoin(owners, ", "),
                       "; call load_skill(name=\"<skill>\", path=\"", name,
                       "\")"));
    }
  }
  return LoadError("unknown_skill", UnknownSkillMessage(visible, name));
}

std::string LoadHint(std::string_view skill) {
  return absl::StrCat("Skill directory: ", skill,
                      "/ — referenced files are loaded with load_skill(name=\"",
                      skill, "\", path=\"<file>\").");
}

// `content` keeps the file/body text verbatim; the tools line is appended only
// when this load registered tools, and never leaves a trailing blank line.
std::string ComposeLoadText(std::string_view skill, std::string_view content,
                            const std::vector<std::string>& tools) {
  std::string text = LoadHint(skill);
  if (!content.empty()) absl::StrAppend(&text, "\n\n", content);
  if (!tools.empty()) {
    if (!content.empty() && text.back() != '\n') text.push_back('\n');
    absl::StrAppend(&text, "\nRegistered tools: ",
                    absl::StrJoin(tools, ", "));
  }
  return text;
}

struct LoadState {
  std::string current_skill;
};

std::string LoadSkill(const SkillLibrary& library,
                      const std::vector<std::string>& visible,
                      LoadState* state, const SkillLoadHook& on_load,
                      std::string_view args_json) {
  const json args = json::parse(args_json, nullptr, false);
  if (!args.is_object()) {
    return ErrorJson("bad_args", "arguments must be a JSON object");
  }
  if (!args.contains("name") || !args["name"].is_string() ||
      args["name"].get<std::string>().empty()) {
    return ErrorJson("bad_args", "'name' must be a non-empty string");
  }
  const std::string name = args["name"].get<std::string>();
  bool has_path = false;
  std::string path;
  if (args.contains("path") && !args["path"].is_null()) {
    if (!args["path"].is_string()) {
      return ErrorJson("bad_args", "'path' must be a string");
    }
    path = args["path"].get<std::string>();
    // Strict-mode tool calls must include every declared property, so a model
    // that only wants the skill body sends path:"" — there is no file to name.
    // A blank path therefore means "no path", not a file request.
    has_path = !absl::StripAsciiWhitespace(path).empty();
  }

  auto outcome = ResolveLoad(library, visible, state->current_skill, name,
                             has_path, path);
  if (!outcome.ok()) return StatusToErrorJson(outcome.status());

  std::string text = ComposeLoadText(outcome->skill, outcome->content,
                                     outcome->tools);
  if (on_load) {
    SkillLoadResult result;
    result.text = text;
    result.tool_names = outcome->tools;
    result.loaded_skill = outcome->skill;
    result.loaded_file = outcome->file;
    result.is_file = outcome->is_file;
    const absl::Status status = on_load(result);
    if (!status.ok()) return StatusToErrorJson(status);
  } else if (!outcome->tools.empty()) {
    // Read-only mode: nothing was registered, so do not claim otherwise.
    text = ComposeLoadText(outcome->skill, outcome->content, {});
  }
  // A failed registration must not move the session's current skill.
  if (!outcome->is_file) state->current_skill = outcome->skill;
  return text;
}

std::string FindSkill(const SkillLibrary& library,
                      const std::vector<std::string>& visible,
                      std::string_view args_json) {
  const json args = json::parse(args_json, nullptr, false);
  if (!args.is_object()) {
    return ErrorJson("bad_args", "arguments must be a JSON object");
  }
  if (!args.contains("query") || !args["query"].is_string()) {
    return ErrorJson("bad_args", "'query' must be a string");
  }
  const std::string query = args["query"].get<std::string>();
  if (absl::StripAsciiWhitespace(query).empty()) {
    return ErrorJson("bad_args", "'query' must not be empty");
  }
  std::size_t limit = static_cast<std::size_t>(kDefaultFindLimit);
  if (args.contains("limit") && !args["limit"].is_null()) {
    if (!args["limit"].is_number_integer()) {
      return ErrorJson("bad_args", "'limit' must be an integer");
    }
    const int value = args["limit"].get<int>();
    if (value < 1) return ErrorJson("bad_args", "'limit' must be at least 1");
    limit = std::min<std::size_t>(static_cast<std::size_t>(value),
                                  static_cast<std::size_t>(kMaxFindLimit));
  }

  json skills = json::array();
  for (const SkillSearchResult& result : library.Search(query, limit, visible)) {
    skills.push_back({{"name", result.name},
                      {"description", result.description},
                      {"matched", result.matched}});
  }
  return json{{"skills", std::move(skills)}}.dump();
}

std::string ReadSkillFile(const SkillLibrary& library,
                          const std::vector<std::string>& visible,
                          std::string_view args_json) {
  const json args = json::parse(args_json, nullptr, false);
  if (!args.is_object()) {
    return ErrorJson("bad_args", "arguments must be a JSON object");
  }
  if (!args.contains("skill") || !args["skill"].is_string()) {
    return ErrorJson("bad_args", "'skill' must be a string");
  }
  const std::string skill_name = args["skill"].get<std::string>();
  const Skill* skill = VisibleSkill(library, visible, skill_name);
  if (skill == nullptr) {
    return ErrorJson("unknown_skill", UnknownSkillMessage(visible, skill_name));
  }
  if (!args.contains("path") || !args["path"].is_string()) {
    return ErrorJson("bad_args", "'path' must be a string");
  }
  auto content = ReadSkillFileContent(*skill, args["path"].get<std::string>());
  if (!content.ok()) return StatusToErrorJson(content.status());
  return *content;
}

}  // namespace

absl::Status SkillToolError(std::string_view code, std::string_view message) {
  return LoadError(code, message);
}

absl::StatusOr<std::vector<std::shared_ptr<agentflow::Tool>>> MakeSkillTools(
    std::shared_ptr<const SkillLibrary> library,
    std::vector<std::string> visible_names, SkillLoadHook on_load) {
  if (visible_names.empty()) return std::vector<std::shared_ptr<agentflow::Tool>>{};
  if (library == nullptr) return absl::InvalidArgumentError("skill library is null");
  const auto selected = library->Select(visible_names);
  if (!selected.ok()) return selected.status();

  const json name_enum = visible_names;
  const json find_schema = {
      {"type", "object"},
      {"properties",
       {{"query",
         {{"type", "string"},
          {"description",
           "Skill name, words from a description, or a reference file name."}}},
        {"limit",
         {{"type", "integer"},
          {"minimum", 1},
          {"maximum", kMaxFindLimit},
          {"description", "Maximum number of skills to return (default 5)."}}}}},
      {"required", json::array({"query"})},
      {"additionalProperties", false}};
  const json load_schema = {
      {"type", "object"},
      {"properties",
       {{"name",
         {{"type", "string"},
          {"description",
           "Skill name, or a reference file name ending in .md."}}},
        {"path",
         {{"type", "string"},
          {"description",
           "Optional file path relative to the skill directory."}}}}},
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
  auto state = std::make_shared<LoadState>();
  std::vector<std::shared_ptr<agentflow::Tool>> tools;
  tools.push_back(std::make_shared<agentflow::NativeFnTool>(
      agentflow::ToolSchema{std::string(kFindSkillToolName),
                            "Search the visible skills by name, description, or "
                            "reference file name. Read-only and side-effect free.",
                            find_schema.dump()},
      [library, visible](std::string_view args, std::string_view,
                         const CancelToken&) -> asio::awaitable<std::string> {
        co_return FindSkill(*library, *visible, args);
      }));
  tools.push_back(std::make_shared<agentflow::NativeFnTool>(
      agentflow::ToolSchema{std::string(kLoadSkillToolName),
                            "Load a skill's full instructions by name, or one of "
                            "its reference files with path. Loads are idempotent.",
                            load_schema.dump()},
      [library, visible, state,
       on_load](std::string_view args, std::string_view,
                const CancelToken&) -> asio::awaitable<std::string> {
        co_return LoadSkill(*library, *visible, state.get(), on_load, args);
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
      "## Skills\n"
      "Load a skill on demand: when it applies to the task, call load_skill(name) "
      "and read its full instructions before acting.\n";
  for (const Skill* skill : skills) {
    std::vector<absl::string_view> words = absl::StrSplit(
        skill->description, absl::ByAnyChar(" \t\r\n"), absl::SkipEmpty());
    absl::StrAppend(&out, "- ", skill->name, ": ", absl::StrJoin(words, " "), "\n");
  }
  return out;
}

}  // namespace agentflow::skills
