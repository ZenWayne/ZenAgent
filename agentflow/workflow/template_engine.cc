#include "agentflow/workflow/template_engine.h"

#include <type_traits>
#include <utility>

#include <absl/strings/str_cat.h>
#include <absl/strings/str_split.h>
#include <absl/time/time.h>

namespace agentflow::workflow {
namespace {

bool KnownHead(std::string_view head) {
  return head == "state" || head == "parent" || head == "tool" ||
         head == "workflow" || head == "now";
}

nlohmann::ordered_json Resolve(const std::vector<std::string>& parts,
                               const EvalContext& ctx) {
  if (parts.empty()) return nullptr;
  const std::string& head = parts[0];
  if (head == "workflow") {
    if (parts.size() < 2) return nullptr;
    if (parts[1] == "name")    return ctx.workflow_name;
    if (parts[1] == "version") return ctx.workflow_version;
    return nullptr;
  }
  if (head == "now") {
    if (parts.size() < 2) return nullptr;
    if (parts[1] == "iso") return absl::FormatTime(ctx.now);
    if (parts[1] == "unix_micros")  return absl::ToUnixMicros(ctx.now);
    if (parts[1] == "unix_seconds") return absl::ToUnixSeconds(ctx.now);
    return nullptr;
  }
  if (head == "tool") {
    if (!ctx.tool_args || parts.size() < 2) return nullptr;
    const nlohmann::ordered_json* cur = ctx.tool_args;
    for (size_t i = 1; i < parts.size(); ++i) {
      if (!cur->is_object() || !cur->contains(parts[i])) return nullptr;
      cur = &cur->at(parts[i]);
    }
    return *cur;
  }
  if (head == "state" || head == "parent") {
    const State* st = (head == "state") ? ctx.state : ctx.parent_state;
    if (!st || parts.size() < 2) return nullptr;
    size_t start = 1;
    if (head == "parent") {
      if (parts.size() < 3 || parts[1] != "state") return nullptr;
      start = 2;
    }
    if (const auto* j = AsJson(*st)) {
      const nlohmann::ordered_json* cur = j;
      for (size_t i = start; i < parts.size(); ++i) {
        if (!cur->is_object() || !cur->contains(parts[i])) return nullptr;
        cur = &cur->at(parts[i]);
      }
      return *cur;
    }
    std::string dotted;
    for (size_t i = start; i < parts.size(); ++i) {
      if (i > start) dotted.push_back('.');
      dotted.append(parts[i]);
    }
    return ReadStringField(*st, dotted);
  }
  return nullptr;
}

std::string Stringify(const nlohmann::ordered_json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_null()) return "";
  return v.dump();
}

}  // namespace

absl::StatusOr<TemplateString> TemplateString::Parse(std::string_view expr) {
  TemplateString out;
  out.source_ = std::string(expr);
  size_t i = 0;
  std::string lit;
  int subst_count = 0;
  while (i < expr.size()) {
    if (i + 1 < expr.size() && expr[i] == '\\' && expr[i + 1] == '{') {
      lit.push_back('{');
      i += 2;
      continue;
    }
    if (i + 1 < expr.size() && expr[i] == '{' && expr[i + 1] == '{') {
      if (!lit.empty()) {
        out.segs_.emplace_back(LiteralSeg{std::move(lit)});
        lit.clear();
      }
      size_t close = expr.find("}}", i + 2);
      if (close == std::string_view::npos) {
        return absl::InvalidArgumentError("unbalanced '{{'");
      }
      std::string_view inner = expr.substr(i + 2, close - (i + 2));
      while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t')) {
        inner.remove_prefix(1);
      }
      while (!inner.empty() && (inner.back()  == ' ' || inner.back()  == '\t')) {
        inner.remove_suffix(1);
      }
      std::vector<std::string> parts = absl::StrSplit(inner, absl::ByChar('.'));
      if (parts.empty() || !KnownHead(parts[0])) {
        return absl::InvalidArgumentError(absl::StrCat(
            "unknown path head '", parts.empty() ? "" : parts[0],
            "' (allowed: state, parent, tool, workflow, now)"));
      }
      out.paths_.push_back(parts);
      out.segs_.emplace_back(PathSeg{std::move(parts)});
      ++subst_count;
      i = close + 2;
      continue;
    }
    if (expr[i] == '}' && i + 1 < expr.size() && expr[i + 1] == '}') {
      return absl::InvalidArgumentError("unmatched '}}'");
    }
    lit.push_back(expr[i]);
    ++i;
  }
  if (!lit.empty()) out.segs_.emplace_back(LiteralSeg{std::move(lit)});
  out.single_substitution_ = subst_count == 1 && out.segs_.size() == 1;
  return out;
}

nlohmann::ordered_json TemplateString::Evaluate(const EvalContext& ctx) const {
  if (single_substitution_) {
    return Resolve(std::get<PathSeg>(segs_[0]).parts, ctx);
  }
  std::string buf;
  for (const auto& seg : segs_) {
    std::visit(
        [&](const auto& s) {
          using T = std::decay_t<decltype(s)>;
          if constexpr (std::is_same_v<T, LiteralSeg>) {
            buf.append(s.text);
          } else {
            buf.append(Stringify(Resolve(s.parts, ctx)));
          }
        },
        seg);
  }
  return buf;
}

}  // namespace agentflow::workflow
