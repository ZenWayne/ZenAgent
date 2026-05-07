# cmake/CompilerFlags.cmake
#
# Sets the C++ standard and warning flags on a target. Despite the historical
# name, also propagates cxx_std_20 (PUBLIC) so consumers compile against C++20.
function(agentflow_apply_warnings target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor
      -Werror=return-type
    )
  endif()
  agentflow_apply_sanitizers(${target})
endfunction()

# Sanitizer options. ASan and TSan are mutually exclusive; UBSan composes
# with either (or neither). Configure-time toggles only.
option(AGENTFLOW_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AGENTFLOW_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(AGENTFLOW_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(AGENTFLOW_ENABLE_ASAN AND AGENTFLOW_ENABLE_TSAN)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together.")
endif()

# Compose the flag list once. Linked into _every_ target via add_link_options
# (the runtime libraries must be present in the final binary), but compile
# flags are applied per-target via agentflow_apply_sanitizers — vendored deps
# (absl, protobuf, gtest) hit constexpr/template issues under UBSan and are
# excluded from instrumentation. Runtime checks in our code still apply.
set(_agentflow_sanitize_compile)
set(_agentflow_sanitize_link)

if(AGENTFLOW_ENABLE_ASAN)
  list(APPEND _agentflow_sanitize_compile -fsanitize=address -fno-omit-frame-pointer)
  list(APPEND _agentflow_sanitize_link -fsanitize=address)
endif()
if(AGENTFLOW_ENABLE_UBSAN)
  list(APPEND _agentflow_sanitize_compile -fsanitize=undefined -fno-omit-frame-pointer)
  list(APPEND _agentflow_sanitize_link -fsanitize=undefined)
endif()
if(AGENTFLOW_ENABLE_TSAN)
  list(APPEND _agentflow_sanitize_compile -fsanitize=thread -fno-omit-frame-pointer)
  list(APPEND _agentflow_sanitize_link -fsanitize=thread)
endif()

# Link options are global — runtime libraries must be linked into the final
# executable for the sanitizers in our code to work, even when third-party
# objects aren't instrumented.
if(_agentflow_sanitize_link)
  add_link_options(${_agentflow_sanitize_link})
endif()

function(agentflow_apply_sanitizers target)
  if(_agentflow_sanitize_compile)
    target_compile_options(${target} PRIVATE ${_agentflow_sanitize_compile})
  endif()
endfunction()
