# Source selection follows the upstream host build.
add_library(zxdb_symbols STATIC
  address_range_map_builder.cc
  arch.cc
  array_type.cc
  base_type.cc
  build_id_index.cc
  code_block.cc
  collection.cc
  compile_unit.cc
  const_value.cc
  data_member.cc
  dwarf_binary_impl.cc
  dwarf_debug_names.cc
  dwarf_die_decoder.cc
  dwarf_die_scanner.cc
  dwarf_expr.cc
  dwarf_expr_eval.cc
  dwarf_lang.cc
  dwarf_location.cc
  dwarf_stack_entry.cc
  dwarf_symbol_factory.cc
  dwarf_tag.cc
  dwarf_unit_impl.cc
  dwo_info.cc
  elf_symbol.cc
  elf_symbol_record.cc
  enumeration.cc
  file_line.cc
  find_line.cc
  function.cc
  function_type.cc
  identifier.cc
  identifier_base.cc
  index.cc
  index_node.cc
  inheritance_path.cc
  inherited_from.cc
  input_location.cc
  lazy_symbol.cc
  line_details.cc
  line_table.cc
  line_table_impl.cc
  loaded_module_symbols.cc
  location.cc
  member_ptr.cc
  modified_type.cc
  module_indexer.cc
  module_symbols.cc
  module_symbols_impl.cc
  namespace.cc
  process_symbols.cc
  source_util.cc
  symbol.cc
  symbol_context.cc
  symbol_data_provider.cc
  symbol_factory.cc
  symbol_utils.cc
  system_symbols.cc
  target_symbols.cc
  template_parameter.cc
  type.cc
  value.cc
  variable.cc
  variable_location.cc
  variant.cc
  variant_part.cc
  visit_scopes.cc
)
target_link_libraries(zxdb_symbols PUBLIC zxdb_common debug_ipc elflib llvm_host rapidjson)
# LLVM 19's ScopedPrinter header destroys an incomplete unique_ptr in a
# default argument under C++23. This translation unit needs only C++20.
if(LLVM_VERSION_MAJOR EQUAL 19)
  set_source_files_properties(dwarf_binary_impl.cc PROPERTIES COMPILE_OPTIONS -std=c++20)
endif()
