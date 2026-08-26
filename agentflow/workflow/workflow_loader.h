#ifndef AGENTFLOW_WORKFLOW_WORKFLOW_LOADER_H_
#define AGENTFLOW_WORKFLOW_WORKFLOW_LOADER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

#include "agentflow/core/event.h"
#include "agentflow/tools/tool_registry.h"
#include "agentflow/workflow/workflow.h"

namespace agentflow::workflow {

inline constexpr uint32_t kCurrentWorkflowSchemaVersion = 1;

// Hosts implement this to provide HMAC keys for signature verification.
class KeyResolver {
 public:
  virtual ~KeyResolver() = default;
  virtual absl::StatusOr<std::string> Resolve(std::string_view key_id) = 0;
};

class WorkflowLoader {
 public:
  struct Options {
    size_t max_json_bytes = 256 * 1024;
    KeyResolver* key_resolver = nullptr;
    bool require_signed = false;
    EventEmitter* trace = nullptr;
    Options() = default;
  };

  [[nodiscard]] static absl::StatusOr<std::shared_ptr<Workflow>> Load(
      std::string_view json_text,
      const ToolRegistry& host_tools,
      const Options& opts);

  [[nodiscard]] static absl::StatusOr<std::shared_ptr<Workflow>> Load(
      std::string_view json_text,
      const ToolRegistry& host_tools) {
    return Load(json_text, host_tools, Options{});
  }

  [[nodiscard]] static absl::StatusOr<std::shared_ptr<Workflow>> LoadFromFile(
      const std::string& path,
      const ToolRegistry& host_tools,
      const Options& opts);

  [[nodiscard]] static absl::StatusOr<std::shared_ptr<Workflow>> LoadFromFile(
      const std::string& path,
      const ToolRegistry& host_tools) {
    return LoadFromFile(path, host_tools, Options{});
  }
};

}  // namespace agentflow::workflow

#endif
