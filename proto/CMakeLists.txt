# proto/CMakeLists.txt
set(PROTO_FILES
  errors.proto
  trace_event.proto
  test_messages.proto
)

add_library(agentflow_proto STATIC ${PROTO_FILES})
target_link_libraries(agentflow_proto PUBLIC protobuf::libprotobuf)

protobuf_generate(
  TARGET agentflow_proto
  IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
  PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
)

target_include_directories(agentflow_proto
  PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

agentflow_apply_warnings(agentflow_proto)
# protobuf-generated code triggers some warnings; relax for this target only:
target_compile_options(agentflow_proto PRIVATE -Wno-shadow)
