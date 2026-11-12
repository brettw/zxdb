# Source selection follows the upstream host build.
add_library(zxdb_debug_adapter STATIC
  async_backtrace_subscription.cc
  context.cc
  data_id.cc
  handlers/request_async_backtrace.cc
  handlers/request_attach.cc
  handlers/request_breakpoint.cc
  handlers/request_console_command.cc
  handlers/request_continue.cc
  handlers/request_data_breakpoint.cc
  handlers/request_evaluate.cc
  handlers/request_function_breakpoint.cc
  handlers/request_launch.cc
  handlers/request_next.cc
  handlers/request_pause.cc
  handlers/request_process.cc
  handlers/request_scopes.cc
  handlers/request_stacktrace.cc
  handlers/request_step_in.cc
  handlers/request_step_out.cc
  handlers/request_terminate.cc
  handlers/request_threads.cc
  handlers/request_tracepoint.cc
  handlers/request_variables.cc
  handlers/request_zxdb_detach.cc
  server.cc
  type_conversions.cc
)
target_link_libraries(zxdb_debug_adapter PUBLIC zxdb_console cppdap)
