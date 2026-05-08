// agentflow/core/errors.h
#ifndef AGENTFLOW_CORE_ERRORS_H_
#define AGENTFLOW_CORE_ERRORS_H_

#include <stdexcept>
#include <string>

namespace agentflow {

class AgentflowError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class GraphCompileError : public AgentflowError {
 public:
  using AgentflowError::AgentflowError;
};

class LlmError : public AgentflowError {
 public:
  using AgentflowError::AgentflowError;
};

class LlmAbortedError : public LlmError {
 public:
  using LlmError::LlmError;
};

class LlmOomError : public LlmError {
 public:
  using LlmError::LlmError;
};

class ToolError : public AgentflowError {
 public:
  using AgentflowError::AgentflowError;
};

class McpTransportError : public ToolError {
 public:
  using ToolError::ToolError;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_ERRORS_H_
