// agentflow/core/edge.h
#ifndef AGENTFLOW_CORE_EDGE_H_
#define AGENTFLOW_CORE_EDGE_H_

#include <string>

namespace agentflow {

struct Edge {
  enum class Condition { ALL, ANY };

  std::string from;
  std::string to;

  // 0  = standard DAG fan-in edge (count must drain to 0)
  // >0 = user-declared cycle/back edge (counter resets between firings;
  //      vacuously satisfied on the destination node's first activation)
  int activation_group = 0;

  Condition condition = Condition::ALL;
};

}  // namespace agentflow

#endif  // AGENTFLOW_CORE_EDGE_H_
