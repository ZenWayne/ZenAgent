# cmake/CompilerFlags.cmake
function(agentflow_apply_warnings target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
      -Werror=return-type
    )
  endif()
endfunction()

option(AGENTFLOW_ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(AGENTFLOW_ENABLE_ASAN)
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
endif()
