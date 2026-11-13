#include "agentflow/skills/skill_library.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"

namespace agentflow::skills {
namespace fs = std::filesystem;
namespace {

constexpr std::size_t kMaxIndexedFilesPerSkill = 200;

absl::Status RootFailure(const SkillRoot& root, std::string_view detail) {
  return absl::FailedPreconditionError(absl::StrCat(root.label, ": ", detail));
}

absl::StatusOr<std::string> ReadWholeFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return absl::FailedPreconditionError(
        absl::StrCat("cannot read ", path.string()));
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  if (in.bad()) {
    return absl::FailedPreconditionError(
        absl::StrCat("cannot read ", path.string()));
  }
  return contents.str();
}

// Every immediate subdirectory of the root, sorted. A missing root is empty.
absl::StatusOr<std::vector<fs::path>> ListSkillDirs(const SkillRoot& root) {
  std::error_code ec;
  const bool exists = fs::exists(root.dir, ec);
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot inspect skills root ",
                                          root.dir.string(), ": ", ec.message()));
  }
  if (!exists) return std::vector<fs::path>{};
  if (!fs::is_directory(root.dir, ec) || ec) {
    return RootFailure(root, absl::StrCat(
        "skills root is not a readable directory: ", root.dir.string(),
        ec ? absl::StrCat(": ", ec.message()) : ""));
  }

  std::vector<fs::path> subdirs;
  fs::directory_iterator it(root.dir, ec), end;
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot list skills root ",
                                          root.dir.string(), ": ", ec.message()));
  }
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    const bool is_dir = it->is_directory(ec);
    if (ec) break;
    if (is_dir) subdirs.push_back(it->path());
  }
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot list skills root ",
                                          root.dir.string(), ": ", ec.message()));
  }
  std::sort(subdirs.begin(), subdirs.end());
  return subdirs;
}

// Indexes the skill directory's direct regular files, skipping SKILL.md and
// every subdirectory: sorted by name, then capped at 200 entries.
absl::Status IndexFlatFiles(const SkillRoot& root, const fs::path& dir,
                            std::vector<std::string>* files) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec), end;
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot list ", dir.string(), ": ",
                                          ec.message()));
  }
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    const std::string name = it->path().filename().string();
    if (name == "SKILL.md") continue;
    const bool is_dir = it->is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (is_dir) continue;
    const bool is_file = it->is_regular_file(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!is_file) continue;
    files->push_back(name);
  }
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot list ", dir.string(), ": ",
                                          ec.message()));
  }
  std::sort(files->begin(), files->end());
  if (files->size() > kMaxIndexedFilesPerSkill) {
    files->resize(kMaxIndexedFilesPerSkill);
  }
  return absl::OkStatus();
}

struct LoadedSkill {
  Skill skill;
  fs::path manifest;
  std::vector<std::string> unknown_fields;
  std::vector<std::string> files;
};

// nullopt when the directory has no SKILL.md; any other problem is an error.
absl::StatusOr<std::optional<LoadedSkill>> LoadSkillDir(const SkillRoot& root,
                                                        const fs::path& dir) {
  const fs::path md = dir / "SKILL.md";
  std::error_code ec;
  const fs::file_status manifest_entry = fs::symlink_status(md, ec);
  if (ec == std::errc::no_such_file_or_directory) {
    return std::optional<LoadedSkill>{};
  }
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot inspect ", md.string(), ": ",
                                          ec.message()));
  }
  if (manifest_entry.type() == fs::file_type::not_found) {
    return std::optional<LoadedSkill>{};
  }

  const fs::file_status manifest_target = fs::status(md, ec);
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot inspect ", md.string(), ": ",
                                          ec.message()));
  }
  if (!fs::is_regular_file(manifest_target)) {
    return RootFailure(root, absl::StrCat("SKILL.md is not a regular file: ",
                                          md.string()));
  }

  auto contents = ReadWholeFile(md);
  if (!contents.ok()) {
    return RootFailure(root, contents.status().message());
  }
  LoadedSkill loaded;
  loaded.manifest = md;
  auto skill = ParseSkillMd(*contents, dir.filename().string(),
                            &loaded.unknown_fields);
  if (!skill.ok()) {
    return absl::InvalidArgumentError(absl::StrCat(
        root.label, ": ", md.string(), ": ", skill.status().message()));
  }
  loaded.skill = std::move(*skill);

  loaded.skill.dir = fs::canonical(dir, ec);
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot resolve ", dir.string(), ": ",
                                          ec.message()));
  }
  const fs::path scripts_dir = dir / "scripts";
  const bool scripts_exist = fs::exists(scripts_dir, ec);
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot inspect scripts directory ",
                                          scripts_dir.string(), ": ", ec.message()));
  }
  loaded.skill.has_scripts = scripts_exist && fs::is_directory(scripts_dir, ec);
  if (ec) {
    return RootFailure(root, absl::StrCat("cannot inspect scripts directory ",
                                          scripts_dir.string(), ": ", ec.message()));
  }
  if (absl::Status status = IndexFlatFiles(root, dir, &loaded.files);
      !status.ok()) {
    return status;
  }
  return loaded;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  absl::AsciiStrToLower(&out);
  return out;
}

std::vector<std::string> QueryTerms(std::string_view query) {
  std::vector<std::string> terms;
  for (absl::string_view term :
       absl::StrSplit(query, absl::ByAnyChar(" \t\r\n"), absl::SkipEmpty())) {
    std::string lower = Lower(term);
    if (std::find(terms.begin(), terms.end(), lower) == terms.end()) {
      terms.push_back(std::move(lower));
    }
  }
  return terms;
}

bool ContainsAll(std::string_view haystack,
                 const std::vector<std::string>& terms) {
  for (const std::string& term : terms) {
    if (haystack.find(term) == std::string_view::npos) return false;
  }
  return true;
}

struct Candidate {
  int rank = 0;
  SkillSearchResult result;
};

}  // namespace

absl::StatusOr<std::shared_ptr<const SkillLibrary>> SkillLibrary::Load(
    const std::vector<SkillRoot>& roots, std::vector<std::string>* warnings) {
  auto library = std::make_shared<SkillLibrary>();
  for (const SkillRoot& root : roots) {
    auto dirs = ListSkillDirs(root);
    if (!dirs.ok()) return dirs.status();
    for (const fs::path& dir : *dirs) {
      auto loaded = LoadSkillDir(root, dir);
      if (!loaded.ok()) return loaded.status();
      if (!loaded->has_value()) continue;
      LoadedSkill& entry = **loaded;

      if (warnings != nullptr) {
        for (const std::string& field : entry.unknown_fields) {
          warnings->push_back(absl::StrCat(
              root.label, ": ", entry.manifest.string(),
              ": ignored unknown frontmatter field '", field, "'"));
        }
        if (entry.skill.tools_from_allowed_tools) {
          warnings->push_back(absl::StrCat(
              root.label, ": ", entry.manifest.string(),
              ": frontmatter 'allowed-tools' is a deprecated alias for 'tools'; "
              "treating its entries as this skill's tools (migration warning)"));
        }
        if (library->skills_.contains(entry.skill.name)) {
          warnings->push_back(absl::StrCat(
              root.label, ": skill '", entry.skill.name,
              "' overrides one from a lower-priority root"));
        }
      }
      const std::string name = entry.skill.name;
      library->files_.insert_or_assign(name, std::move(entry.files));
      library->skills_.insert_or_assign(name, std::move(entry.skill));
    }
  }
  return std::shared_ptr<const SkillLibrary>(std::move(library));
}

const Skill* SkillLibrary::Find(std::string_view name) const {
  const auto it = skills_.find(name);
  return it == skills_.end() ? nullptr : &it->second;
}

std::vector<const Skill*> SkillLibrary::All() const {
  std::vector<const Skill*> skills;
  skills.reserve(skills_.size());
  for (const auto& [name, skill] : skills_) skills.push_back(&skill);
  return skills;
}

absl::StatusOr<std::vector<const Skill*>> SkillLibrary::Select(
    const std::vector<std::string>& names) const {
  std::vector<const Skill*> selected;
  std::map<std::string_view, bool, std::less<>> seen;
  for (const std::string& name : names) {
    if (!seen.emplace(name, true).second) {
      return absl::InvalidArgumentError(
          absl::StrCat("skill '", name, "' is listed twice"));
    }
    const Skill* skill = Find(name);
    if (skill == nullptr) {
      return absl::NotFoundError(absl::StrCat("unknown skill '", name, "'"));
    }
    selected.push_back(skill);
  }
  return selected;
}

const std::vector<std::string>& SkillLibrary::Files(const Skill& skill) const {
  static const std::vector<std::string> kEmpty;
  const auto it = files_.find(skill.name);
  return it == files_.end() ? kEmpty : it->second;
}

const std::string* SkillLibrary::FindFile(const Skill& skill,
                                          std::string_view name) const {
  const auto it = files_.find(skill.name);
  if (it == files_.end()) return nullptr;
  for (const std::string& file : it->second) {
    if (file == name) return &file;
  }
  return nullptr;
}

std::vector<SkillSearchResult> SkillLibrary::Search(
    std::string_view query, std::size_t limit,
    const std::vector<std::string>& visible_skills) const {
  std::vector<SkillSearchResult> results;
  if (limit == 0) return results;
  const std::string lowered_query = Lower(absl::StripAsciiWhitespace(query));
  if (lowered_query.empty()) return results;
  const std::vector<std::string> terms = QueryTerms(lowered_query);

  std::vector<Candidate> candidates;
  for (const auto& [name, skill] : skills_) {
    if (!visible_skills.empty() &&
        std::find(visible_skills.begin(), visible_skills.end(), name) ==
            visible_skills.end()) {
      continue;
    }
    Candidate candidate;
    candidate.result.name = name;
    candidate.result.description = skill.description;
    if (lowered_query == name) {
      candidate.rank = 1;
    } else if (absl::StartsWith(name, lowered_query)) {
      candidate.rank = 2;
    } else if (name.find(lowered_query) != std::string::npos) {
      candidate.rank = 3;
    } else if (ContainsAll(Lower(skill.description), terms)) {
      candidate.rank = 4;
    } else {
      const auto it = files_.find(name);
      if (it != files_.end()) {
        for (const std::string& file : it->second) {
          if (ContainsAll(Lower(file), terms)) {
            candidate.rank = 5;
            candidate.result.matched = file;
            break;
          }
        }
      }
    }
    if (candidate.rank != 0) candidates.push_back(std::move(candidate));
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return std::tie(a.rank, a.result.name, a.result.matched) <
                     std::tie(b.rank, b.result.name, b.result.matched);
            });
  if (candidates.size() > limit) candidates.resize(limit);
  results.reserve(candidates.size());
  for (Candidate& candidate : candidates) {
    results.push_back(std::move(candidate.result));
  }
  return results;
}

}  // namespace agentflow::skills
