#include "agentflow/skills/skill_library.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include "absl/status/status.h"
#include "absl/strings/str_cat.h"

namespace agentflow::skills {
namespace fs = std::filesystem;
namespace {

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

}  // namespace

absl::StatusOr<std::shared_ptr<const SkillLibrary>> SkillLibrary::Load(
    const std::vector<SkillRoot>& roots, std::vector<std::string>* warnings) {
  auto library = std::make_shared<SkillLibrary>();
  for (const SkillRoot& root : roots) {
    std::error_code ec;
    const bool exists = fs::exists(root.dir, ec);
    if (ec) {
      return absl::FailedPreconditionError(absl::StrCat(
          root.label, ": cannot inspect skills root ", root.dir.string(),
          ": ", ec.message()));
    }
    if (!exists) continue;
    if (!fs::is_directory(root.dir, ec) || ec) {
      return absl::FailedPreconditionError(absl::StrCat(
          root.label, ": skills root is not a readable directory: ",
          root.dir.string(), ec ? absl::StrCat(": ", ec.message()) : ""));
    }

    std::vector<fs::path> subdirs;
    fs::directory_iterator it(root.dir, ec), end;
    if (ec) {
      return absl::FailedPreconditionError(absl::StrCat(
          root.label, ": cannot list skills root ", root.dir.string(),
          ": ", ec.message()));
    }
    for (; it != end; it.increment(ec)) {
      if (ec) break;
      const bool is_dir = it->is_directory(ec);
      if (ec) break;
      if (is_dir) subdirs.push_back(it->path());
    }
    if (ec) {
      return absl::FailedPreconditionError(absl::StrCat(
          root.label, ": cannot list skills root ", root.dir.string(),
          ": ", ec.message()));
    }
    std::sort(subdirs.begin(), subdirs.end());

    for (const fs::path& dir : subdirs) {
      const fs::path md = dir / "SKILL.md";
      const fs::file_status manifest_entry = fs::symlink_status(md, ec);
      if (ec == std::errc::no_such_file_or_directory) {
        ec.clear();
        continue;
      }
      if (ec) {
        return absl::FailedPreconditionError(absl::StrCat(
            root.label, ": cannot inspect ", md.string(), ": ", ec.message()));
      }
      if (manifest_entry.type() == fs::file_type::not_found) continue;

      const fs::file_status manifest_target = fs::status(md, ec);
      if (ec) {
        return absl::FailedPreconditionError(absl::StrCat(
            root.label, ": cannot inspect ", md.string(), ": ", ec.message()));
      }
      if (!fs::is_regular_file(manifest_target)) {
        return absl::FailedPreconditionError(absl::StrCat(
            root.label, ": SKILL.md is not a regular file: ", md.string()));
      }

      auto contents = ReadWholeFile(md);
      if (!contents.ok()) {
        return absl::FailedPreconditionError(
            absl::StrCat(root.label, ": ", contents.status().message()));
      }
      std::vector<std::string> unknown_fields;
      auto skill = ParseSkillMd(*contents, dir.filename().string(), &unknown_fields);
      if (!skill.ok()) {
        return absl::InvalidArgumentError(absl::StrCat(
            root.label, ": ", md.string(), ": ", skill.status().message()));
      }
      skill->dir = fs::canonical(dir, ec);
      if (ec) {
        return absl::FailedPreconditionError(absl::StrCat(
            root.label, ": cannot resolve ", dir.string(), ": ", ec.message()));
      }
      const fs::path scripts_dir = dir / "scripts";
      const bool scripts_exist = fs::exists(scripts_dir, ec);
      if (ec) {
        return absl::FailedPreconditionError(absl::StrCat(
            root.label, ": cannot inspect scripts directory ",
            scripts_dir.string(), ": ", ec.message()));
      }
      skill->has_scripts = scripts_exist && fs::is_directory(scripts_dir, ec);
      if (ec) {
        return absl::FailedPreconditionError(absl::StrCat(
            root.label, ": cannot inspect scripts directory ",
            scripts_dir.string(), ": ", ec.message()));
      }

      if (warnings != nullptr) {
        for (const std::string& field : unknown_fields) {
          warnings->push_back(absl::StrCat(
              root.label, ": ", md.string(),
              ": ignored unknown frontmatter field '", field, "'"));
        }
        if (library->skills_.find(skill->name) != library->skills_.end()) {
          warnings->push_back(absl::StrCat(
              root.label, ": skill '", skill->name,
              "' overrides one from a lower-priority root"));
        }
      }
      std::string name = skill->name;
      library->skills_.insert_or_assign(std::move(name), std::move(*skill));
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
