cmake_minimum_required(VERSION 3.20)
project(agentflow LANGUAGES CXX VERSION 0.1.0)

option(AGENTFLOW_BUILD_TESTS "Build unit tests" ON)
option(AGENTFLOW_BUILD_EXAMPLES "Build example binaries" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
include(CompilerFlags)
include(Dependencies)

add_subdirectory(proto)
add_subdirectory(agentflow)

if(AGENTFLOW_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(AGENTFLOW_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
