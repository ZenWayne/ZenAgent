#include "agentflow/workflow/workflow_registry.h"

#include <utility>

namespace agentflow::workflow {

std::shared_ptr<Workflow> WorkflowRegistry::Register(
    std::shared_ptr<Workflow> wf) {
  if (!wf) return nullptr;
  EventEmitter* trace = nullptr;
  std::string name, version;
  bool signed_value = false;
  std::string key_id;
  {
    std::lock_guard<std::mutex> lk(mu_);
    name = wf->name();
    version = wf->version();
    entries_[name][version] = {wf, absl::Now()};
    latest_version_[name] = version;
    trace = opts_.trace;
    signed_value = wf->spec().has_signing();
    key_id = wf->spec().signing().key_id();
  }
  // Emit outside the lock to avoid holding mu_ during external callbacks.
  if (trace) {
    trace->EmitWorkflowRegistered(name, version, signed_value, key_id);
  }
  return wf;
}

std::shared_ptr<Workflow> WorkflowRegistry::GetLatest(
    std::string_view name) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = latest_version_.find(std::string(name));
  if (it == latest_version_.end()) return nullptr;
  auto bit = entries_.find(it->first);
  if (bit == entries_.end()) return nullptr;
  auto vit = bit->second.find(it->second);
  if (vit == bit->second.end()) return nullptr;
  return vit->second.wf;
}

std::shared_ptr<Workflow> WorkflowRegistry::Get(
    std::string_view name, std::string_view version) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto bit = entries_.find(std::string(name));
  if (bit == entries_.end()) return nullptr;
  auto vit = bit->second.find(std::string(version));
  if (vit == bit->second.end()) return nullptr;
  return vit->second.wf;
}

bool WorkflowRegistry::Unregister(std::string_view name_sv,
                                    std::string_view version_sv) {
  EventEmitter* trace = nullptr;
  std::string name(name_sv), version(version_sv);
  bool erased = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto bit = entries_.find(name);
    if (bit == entries_.end()) return false;
    erased = bit->second.erase(version) > 0;
    if (!erased) return false;
    // If we removed the latest, promote the lex-greatest remaining version,
    // or drop the bucket entirely.
    auto lit = latest_version_.find(name);
    if (lit != latest_version_.end() && lit->second == version) {
      if (bit->second.empty()) {
        latest_version_.erase(lit);
        entries_.erase(bit);
      } else {
        lit->second = bit->second.rbegin()->first;
      }
    } else if (bit->second.empty()) {
      // Removed a non-latest, bucket happens to be empty (shouldn't happen
      // unless multiple versions get removed; defensive).
      entries_.erase(bit);
    }
    trace = opts_.trace;
  }
  if (trace) trace->EmitWorkflowUnregistered(name, version);
  return erased;
}

std::vector<WorkflowRegistry::Entry> WorkflowRegistry::List() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Entry> out;
  for (const auto& [name, bucket] : entries_) {
    for (const auto& [ver, ent] : bucket) {
      out.push_back({name, ver, ent.registered_at});
    }
  }
  return out;
}

}  // namespace agentflow::workflow
