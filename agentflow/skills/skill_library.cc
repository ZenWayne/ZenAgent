#include "agentflow/skills/skill_library.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace agentflow::skills {
namespace fs = std::filesystem;
namespace {

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

struct LoadedSkill {
  Skill skill;
  fs::path manifest;
  std::vector<std::string> unknown_fields;
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
  return loaded;
}

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
        if (library->skills_.contains(entry.skill.name)) {
          warnings->push_back(absl::StrCat(
              root.label, ": skill '", entry.skill.name,
              "' overrides one from a lower-priority root"));
        }
      }
      std::string name = entry.skill.name;
      library->skills_.insert_or_assign(std::move(name), std::move(entry.skill));
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

}  // namespace agentflow::skills
