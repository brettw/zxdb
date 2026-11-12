# Source selection follows the upstream host build.
add_library(zxdb_format STATIC
  async_output_buffer.cc
  format.cc
  format_name.cc
  output_buffer.cc
  string_util.cc
)
target_link_libraries(zxdb_format PUBLIC zxdb_expr zxdb_symbols)
