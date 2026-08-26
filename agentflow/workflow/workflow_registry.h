#ifndef AGENTFLOW_WORKFLOW_WORKFLOW_REGISTRY_H_
#define AGENTFLOW_WORKFLOW_WORKFLOW_REGISTRY_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "absl/time/time.h"

#include "agentflow/core/event.h"
#include "agentflow/workflow/workflow.h"

namespace agentflow::workflow {

// Thread-safe registry holding multiple versions per workflow name.
// Hot-update semantics: Register(v2) does NOT evict v1 — in-flight Runs that
// captured shared_ptr<Workflow> v1 keep running v1. GetLatest(name) returns
// the most-recently-registered version. Get(name, version) pins to a specific
// version.
class WorkflowRegistry {
 public:
  struct Options { EventEmitter* trace = nullptr; };
  WorkflowRegistry() = default;
  explicit WorkflowRegistry(Options opts) : opts_(opts) {}

  std::shared_ptr<Workflow> Register(std::shared_ptr<Workflow> wf);
  std::shared_ptr<Workflow> GetLatest(std::string_view name) const;
  std::shared_ptr<Workflow> Get(std::string_view name,
                                  std::string_view version) const;
  bool Unregister(std::string_view name, std::string_view version);

  struct Entry { std::string name, version; absl::Time registered_at; };
  std::vector<Entry> List() const;

 private:
  mutable std::mutex mu_;
  struct VersionedEntry {
    std::shared_ptr<Workflow> wf;
    absl::Time registered_at;
  };
  // name → (version → entry). Sorted version map for stable iteration.
  std::unordered_map<std::string,
                      std::map<std::string, VersionedEntry>> entries_;
  // name → latest version string (most recently registered).
  std::unordered_map<std::string, std::string> latest_version_;
  Options opts_;
};

}  // namespace agentflow::workflow

#endif
