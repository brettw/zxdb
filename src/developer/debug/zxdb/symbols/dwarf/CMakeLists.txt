add_library(zxdb_dwarf STATIC
  abbrev_table.cc
  aranges_reader.cc
  data_reader.cc
  debug_names_reader.cc
  die_table.cc
  form_encoding.cc
  form_value.cc
  range_list_reader.cc
  unit_context.cc
  unit_header.cc
)
target_link_libraries(zxdb_dwarf PUBLIC zxdb_common)
target_compile_options(zxdb_dwarf PUBLIC -fno-rtti)
