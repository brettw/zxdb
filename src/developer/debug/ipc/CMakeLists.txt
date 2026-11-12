# Source selection follows the upstream host build.
add_library(debug_ipc STATIC
  automation_instruction.cc
  decode_exception.cc
  filter_utils.cc
  message_reader.cc
  message_writer.cc
  protocol.cc
  records.cc
  unwinder_support.cc
)
target_link_libraries(debug_ipc PUBLIC debug::shared unwinder)
