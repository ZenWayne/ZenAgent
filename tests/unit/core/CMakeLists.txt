# tests/unit/core/CMakeLists.txt
include(GoogleTest)

function(agentflow_add_test name)
  add_executable(${name} ${name}.cc)
  target_link_libraries(${name}
    PRIVATE
      agentflow_core
      GTest::gtest
      GTest::gtest_main)
  agentflow_apply_warnings(${name})
  gtest_discover_tests(${name})
endfunction()

agentflow_add_test(errors_test)
