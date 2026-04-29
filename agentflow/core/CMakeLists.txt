# agentflow/core/CMakeLists.txt
add_library(agentflow_core
  errors.cc
  cancel.cc
)

target_include_directories(agentflow_core
  PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(agentflow_core
  PUBLIC
    agentflow_proto
    asio
    absl::status
    absl::strings)

agentflow_apply_warnings(agentflow_core)
