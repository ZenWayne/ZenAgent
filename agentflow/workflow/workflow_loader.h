#ifndef AGENTFLOW_WORKFLOW_WORKFLOW_LOADER_H_
#define AGENTFLOW_WORKFLOW_WORKFLOW_LOADER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <absl/status/statusor.h>
#include <asio/awaitable.hpp>

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

  // Like Load, but also connects any top-level `mcp_servers` declared in the
  // JSON, registering each server's tools into `registry` under the namespace
  // `<id>.<remote>`. Servers that fail to connect are skipped (degrade); agent
  // `tools[]` entries whose prefix names a skipped server are dropped. Requires
  // an MCP-aware ToolRegistry (io_context ctor). lazy_start in JSON is ignored.
  [[nodiscard]] static asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>>
  LoadAndAttach(std::string_view json_text, ToolRegistry& registry,
                Options opts);

  [[nodiscard]] static asio::awaitable<absl::StatusOr<std::shared_ptr<Workflow>>>
  LoadAndAttach(std::string_view json_text, ToolRegistry& registry) {
    return LoadAndAttach(json_text, registry, Options{});
  }
};

}  // namespace agentflow::workflow

#endif
