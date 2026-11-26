add_library(zxdb_dwarf STATIC
  abbrev_table.cc
  data_reader.cc
  unit_header.cc
)
target_link_libraries(zxdb_dwarf PUBLIC zxdb_common)
target_compile_options(zxdb_dwarf PUBLIC -fno-rtti)
