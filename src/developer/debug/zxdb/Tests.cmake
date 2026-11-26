find_package(GTest CONFIG REQUIRED)
add_executable(zxdb_tests
  client/minidump_memory_unittest.cc
  "${PROJECT_SOURCE_DIR}/third_party/crashpad/src/snapshot/test/test_module_snapshot.cc"
  ../ipc/automation_instruction_unittests.cc
  ../ipc/decode_exception_unittests.cc
  ../ipc/filter_utils_unittests.cc
  ../ipc/message_unittests.cc
  ../ipc/protocol_unittests.cc
  client/analytics_event_unittest.cc
  client/breakpoint_impl_unittest.cc
  client/call_site_symbol_data_provider_unittest.cc
  client/client_eval_context_impl_unittest.cc
  client/disassembler_unittest.cc
  client/download_manager_unittest.cc
  client/elf_memory_region_unittest.cc
  client/fdf_dispatcher_task_provider_unittest.cc
  client/filter_unittest.cc
  client/finish_physical_frame_thread_controller_unittest.cc
  client/finish_thread_controller_unittest.cc
  client/frame_fingerprint_unittest.cc
  client/frame_impl_unittest.cc
  client/fuchsia_async_rust_task_provider_unittest.cc
  client/function_thread_controller_unittest.cc
  client/inline_thread_controller_test.cc
  client/map_setting_store_unittest.cc
  client/memory_dump_unittest.cc
  client/minidump_unittest.cc
  client/mock_breakpoint.cc
  client/mock_frame.cc
  client/mock_process.cc
  client/mock_remote_api.cc
  client/mock_target.cc
  client/pretty_frame_glob_unittest.cc
  client/pretty_stack_manager_unittest.cc
  client/process_impl_unittest.cc
  client/process_symbol_data_provider_unittest.cc
  client/remote_api_test.cc
  client/session_unittest.cc
  client/setting_schema_unittest.cc
  client/setting_value_unittest.cc
  client/source_file_provider_impl_unittest.cc
  client/stack_unittest.cc
  client/step_into_specific_thread_controller_unittest.cc
  client/step_into_thread_controller_unittest.cc
  client/step_over_thread_controller_unittest.cc
  client/step_thread_controller_unittest.cc
  client/step_through_plt_thread_controller_unittest.cc
  client/substatement_unittest.cc
  client/symbol_server_unittest.cc
  client/system_unittest.cc
  client/target_impl_unittest.cc
  client/test_thread_observer.cc
  client/thread_controller_test.cc
  client/thread_controller_unittest.cc
  client/thread_impl_unittest.cc
  client/tracepoint_capture_unittest.cc
  client/tracepoint_unittest.cc
  client/until_thread_controller_unittest.cc
  common/adapters_unittest.cc
  common/address_ranges_unittest.cc
  common/cache_dir_unittest.cc
  common/checked_math_unittest.cc
  common/curl_unittest.cc
  common/data_extractor_unittest.cc
  common/err_or_unittest.cc
  common/file_util_unittest.cc
  common/fuzzy_matcher_unittest.cc
  common/inet_util_unittest.cc
  common/join_callbacks_unittest.cc
  common/leb_unittest.cc
  common/tagged_data_unittest.cc
  console/analyze_memory_unittest.cc
  console/command_context_unittest.cc
  console/command_parser_unittest.cc
  console/command_sequence_unittest.cc
  console/command_utils_unittest.cc
  console/commands/verb_aspace_unittest.cc
  console/commands/verb_attach_unittest.cc
  console/commands/verb_break_unittest.cc
  console/commands/verb_clear_unittest.cc
  console/commands/verb_detach_unittest.cc
  console/commands/verb_disassemble_unittest.cc
  console/commands/verb_display_unittest.cc
  console/commands/verb_down_unittest.cc
  console/commands/verb_finish_unittest.cc
  console/commands/verb_list_unittest.cc
  console/commands/verb_new_rm_unittest.cc
  console/commands/verb_print_unittest.cc
  console/commands/verb_ps_unittest.cc
  console/commands/verb_quit_unittest.cc
  console/commands/verb_savedump_unittest.cc
  console/commands/verb_stack_data_unittest.cc
  console/commands/verb_stack_usage_unittest.cc
  console/commands/verb_status_unittest.cc
  console/commands/verb_steps_unittest.cc
  console/commands/verb_sym_info_unittest.cc
  console/commands/verb_sym_stat_unittest.cc
  console/commands/verb_tracepoint_unittest.cc
  console/commands/verb_up_unittest.cc
  console/commands/verb_watch_unittest.cc
  console/console_context_unittest.cc
  console/console_impl_unittest.cc
  console/console_test.cc
  console/format_async_task_unittest.cc
  console/format_context_unittest.cc
  console/format_exception_unittest.cc
  console/format_filter_unittest.cc
  console/format_frame_unittest.cc
  console/format_handle_unittest.cc
  console/format_location_unittest.cc
  console/format_memory_unittest.cc
  console/format_register_unittest.cc
  console/format_settings_unittest.cc
  console/format_symbol_unittest.cc
  console/format_table_unittest.cc
  console/format_target_unittest.cc
  console/format_thread_unittest.cc
  console/format_tracepoint_unittest.cc
  console/input_location_parser_unittest.cc
  console/mock_console.cc
  console/nouns_unittest.cc
  console/script_runner_unittest.cc
  console/string_formatters_unittest.cc
  console/test_failure_stack_matcher_unittest.cc
  console/verbs_settings_unittest.cc
  debug_adapter/async_backtrace_subscription_unittest.cc
  debug_adapter/context_test.cc
  debug_adapter/context_unittest.cc
  debug_adapter/data_id_unittest.cc
  debug_adapter/handlers/request_async_backtrace_unittest.cc
  debug_adapter/handlers/request_attach_unittest.cc
  debug_adapter/handlers/request_breakpoint_unittest.cc
  debug_adapter/handlers/request_console_command_unittest.cc
  debug_adapter/handlers/request_continue_unittest.cc
  debug_adapter/handlers/request_data_breakpoint_unittest.cc
  debug_adapter/handlers/request_evaluate_unittest.cc
  debug_adapter/handlers/request_function_breakpoint_unittest.cc
  debug_adapter/handlers/request_launch_unittest.cc
  debug_adapter/handlers/request_next_unittest.cc
  debug_adapter/handlers/request_pause_unittest.cc
  debug_adapter/handlers/request_process_unittest.cc
  debug_adapter/handlers/request_scopes_unittest.cc
  debug_adapter/handlers/request_stacktrace_unittest.cc
  debug_adapter/handlers/request_step_in_unittest.cc
  debug_adapter/handlers/request_step_out_unittest.cc
  debug_adapter/handlers/request_terminate_unittest.cc
  debug_adapter/handlers/request_threads_unittest.cc
  debug_adapter/handlers/request_tracepoint_unittest.cc
  debug_adapter/handlers/request_variables_unittest.cc
  debug_adapter/handlers/request_zxdb_detach_unittest.cc
  debug_adapter/server_unittest.cc
  expr/bitfield_unittest.cc
  expr/builtin_types_unittest.cc
  expr/cast_unittest.cc
  expr/eval_context_impl_unittest.cc
  expr/eval_dwarf_expr_unittest.cc
  expr/eval_operators_unittest.cc
  expr/eval_test_support.cc
  expr/expr_language_unittest.cc
  expr/expr_node_unittest.cc
  expr/expr_parser_unittest.cc
  expr/expr_tokenizer_unittest.cc
  expr/expr_unittest.cc
  expr/expr_value_source_unittest.cc
  expr/expr_value_unittest.cc
  expr/find_name_unittest.cc
  expr/format_test_support.cc
  expr/format_unittest.cc
  expr/identifier_glob_unittest.cc
  expr/index_walker_unittest.cc
  expr/mock_eval_context.cc
  expr/mock_expr_node.cc
  expr/number_parser_unittest.cc
  expr/operator_keyword_unittest.cc
  expr/parse_special_identifier_unittest.cc
  expr/parse_string_unittest.cc
  expr/parsed_identifier_unittest.cc
  expr/permissive_input_location_unittest.cc
  expr/pretty_rust_tuple_unittest.cc
  expr/pretty_std_string_unittest.cc
  expr/pretty_type_manager_unittest.cc
  expr/pretty_type_unittest.cc
  expr/resolve_array_unittest.cc
  expr/resolve_base_unittest.cc
  expr/resolve_collection_unittest.cc
  expr/resolve_ptr_ref_unittest.cc
  expr/resolve_type_unittest.cc
  expr/resolve_variant_unittest.cc
  expr/return_value_unittest.cc
  expr/template_type_extractor_unittest.cc
  expr/variable_decl_unittest.cc
  expr/vector_register_format_unittest.cc
  expr/x87_float_unittest.cc
  expr/virtual_base_test_setup.cc
  expr/virtual_inheritance_test_setup.cc
  expr/vm_exec_unittest.cc
  expr/vm_stream_unittest.cc
  format/async_output_buffer_test_util.cc
  format/async_output_buffer_unittest.cc
  format/format_name_unittest.cc
  format/format_unittest.cc
  format/output_buffer_unittest.cc
  format/string_util_unittest.cc
  symbols/address_range_map_builder_unittest.cc
  symbols/address_range_map_unittest.cc
  symbols/build_id_index_unittest.cc
  symbols/code_block_unittest.cc
  symbols/collection_unittest.cc
  symbols/const_value_unittest.cc
  symbols/dwarf/abbrev_table_unittest.cc
  symbols/dwarf/aranges_reader_unittest.cc
  symbols/dwarf/data_reader_unittest.cc
  symbols/dwarf/dwarf_section_builder.cc
  symbols/dwarf/dwarf_section_builder_unittest.cc
  symbols/dwarf/form_value_unittest.cc
  symbols/dwarf/range_list_reader_unittest.cc
  symbols/dwarf/unit_context_unittest.cc
  symbols/dwarf/unit_header_unittest.cc
  symbols/dwarf_abstract_child_iterator_unittest.cc
  symbols/dwarf_binary_impl_unittest.cc
  symbols/dwarf_debug_names_unittest.cc
  symbols/dwarf_attr_unittest.cc
  symbols/dwarf_die_decoder_unittest.cc
  symbols/dwarf_die_ref_unittest.cc
  symbols/dwarf_expr_eval_unittest.cc
  symbols/dwarf_form_unittest.cc
  symbols/dwarf_lang_unittest.cc
  symbols/dwarf_location_unittest.cc
  symbols/dwarf_op_unittest.cc
  symbols/dwarf_symbol_factory_unittest.cc
  symbols/dwarf_tag_unittest.cc
  symbols/dwarf_test_util.cc
  symbols/elf_symbol_record_unittest.cc
  symbols/elf_symbol_unittest.cc
  symbols/find_line_unittest.cc
  symbols/function_type_unittest.cc
  symbols/identifier_unittest.cc
  symbols/index_node_unittest.cc
  symbols/index_test_support.cc
  symbols/index_unittest.cc
  symbols/line_file_table_unittest.cc
  symbols/line_program_binary_unittest.cc
  symbols/line_program_test_util.cc
  symbols/line_program_unittest.cc
  symbols/line_table_cache_unittest.cc
  symbols/line_table_test_support.cc
  symbols/line_table_unittest.cc
  symbols/loaded_module_symbols_unittest.cc
  symbols/member_ptr_unittest.cc
  symbols/mock_module_symbols.cc
  symbols/mock_source_file_provider.cc
  symbols/mock_symbol_data_provider.cc
  symbols/mock_symbol_data_provider_unittest.cc
  symbols/modified_type_unittest.cc
  symbols/module_symbols_impl_unittest.cc
  symbols/process_symbols_test_setup.cc
  symbols/process_symbols_unittest.cc
  symbols/source_file_indexer_unittest.cc
  symbols/source_util_unittest.cc
  symbols/symbol_data_provider_unittest.cc
  symbols/symbol_unittest.cc
  symbols/symbol_utils_unittest.cc
  symbols/target_symbols_unittest.cc
  symbols/test_symbol_module.cc
  symbols/type_test_support.cc
  symbols/variable_location_unittest.cc
  symbols/variable_test_support.cc
  symbols/visit_scopes_unittest.cc
)
target_link_libraries(zxdb_tests PRIVATE zxdb_debug_adapter debug_shared_test_support GTest::gtest_main GTest::gmock)
set_target_properties(zxdb_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(ZXDB_INDEX_BENCHMARK_BINARY "" CACHE FILEPATH "Optional binary for the symbol indexing benchmark")
if(ZXDB_INDEX_BENCHMARK_BINARY)
  target_compile_definitions(zxdb_tests PRIVATE
    ZXDB_INDEX_BENCHMARK_BINARY="${ZXDB_INDEX_BENCHMARK_BINARY}")
endif()

add_library(zxdb_symbol_test SHARED
  symbols/test_data/cpp/line_lookup_symbol_test.cc
  symbols/test_data/cpp/type_test.cc
  symbols/test_data/cpp/zxdb_symbol_test.cc
  symbols/test_data/cpp/zxdb_symbol_test2.cc
)
target_include_directories(zxdb_symbol_test PRIVATE "${PROJECT_SOURCE_DIR}")
target_compile_options(zxdb_symbol_test PRIVATE -O0 -gdwarf-5 -gpubnames -fstandalone-debug
  -fdebug-compilation-dir=. "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=../..")
target_link_options(zxdb_symbol_test PRIVATE -Wl,--build-id -Wl,--compress-debug-sections=zstd)
set_target_properties(zxdb_symbol_test PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/test_data/zxdb"
  SUFFIX ".targetso")
add_dependencies(zxdb_tests zxdb_symbol_test)

# Line table fixtures. The same small source builds with different compilers, DWARF versions, and
# linkers. line_program_binary_unittest.cc decodes each one and checks the rows for the lines of the
# fixture source. GCC and LLD fixtures are optional.
set(line_table_fixture_dir "${PROJECT_BINARY_DIR}/test_data/zxdb/line_table")
set(line_table_fixture_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/line_table/line_table_fixture.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/line_table/line_table_fixture_dead.cc")
set(line_table_fixture_outputs)
function(zxdb_add_line_table_fixture name compiler)
  set(output "${line_table_fixture_dir}/${name}.so")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${line_table_fixture_dir}"
    COMMAND "${compiler}" ${ARGN} -g -shared -fPIC -fvisibility=hidden -ffunction-sections
      -Wl,--build-id "-I${PROJECT_SOURCE_DIR}" "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=../.."
      -o "${output}" ${line_table_fixture_sources}
    DEPENDS ${line_table_fixture_sources}
      "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/line_table/line_table_fixture.h"
    VERBATIM)
  list(APPEND line_table_fixture_outputs "${output}")
  set(line_table_fixture_outputs "${line_table_fixture_outputs}" PARENT_SCOPE)
endfunction()
zxdb_add_line_table_fixture(clang_dwarf4 "${CMAKE_CXX_COMPILER}" -gdwarf-4 -O0)
zxdb_add_line_table_fixture(clang_dwarf5_gc "${CMAKE_CXX_COMPILER}" -gdwarf-5 -O2
  -fuse-ld=bfd -Wl,--gc-sections)
zxdb_add_line_table_fixture(clang_dwarf64 "${CMAKE_CXX_COMPILER}" -gdwarf-5 -gdwarf64 -O1)
find_program(ZXDB_LLD NAMES ld.lld HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(ZXDB_LLD)
  # Writes the all-ones tombstone instead of 0 for removed code in .debug_line.
  zxdb_add_line_table_fixture(clang_dwarf4_lld_tombstone "${CMAKE_CXX_COMPILER}" -gdwarf-4 -O2
    "--ld-path=${ZXDB_LLD}" -Wl,--gc-sections
    -Wl,-z,dead-reloc-in-nonalloc=.debug_line=0xffffffffffffffff)
endif()
find_program(ZXDB_GXX NAMES g++)
if(ZXDB_GXX)
  zxdb_add_line_table_fixture(gcc_dwarf4_gc "${ZXDB_GXX}" -gdwarf-4 -O2 -Wl,--gc-sections)
  zxdb_add_line_table_fixture(gcc_dwarf5 "${ZXDB_GXX}" -gdwarf-5 -O0)
endif()
# GCC binaries with type units, in .debug_types for DWARF 4 and in .debug_info for DWARF 5. GCC
# references the types in type units directly with DW_FORM_ref_sig8. dwarf_symbol_factory_unittest.cc
# resolves these references. The fixtures are optional.
set(type_unit_fixture_dir "${PROJECT_BINARY_DIR}/test_data/zxdb/type_unit")
set(type_unit_fixture_source
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/type_unit/type_unit_fixture.cc")
set(type_unit_fixture_outputs)
if(ZXDB_GXX)
  foreach(version 4 5)
    set(output "${type_unit_fixture_dir}/gcc_dwarf${version}.so")
    add_custom_command(
      OUTPUT "${output}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${type_unit_fixture_dir}"
      COMMAND "${ZXDB_GXX}" -g -gdwarf-${version} -fdebug-types-section -shared -fPIC
        -Wl,--build-id "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=../.." -o "${output}"
        "${type_unit_fixture_source}"
      DEPENDS "${type_unit_fixture_source}"
      VERBATIM)
    list(APPEND type_unit_fixture_outputs "${output}")
  endforeach()
endif()
add_custom_target(zxdb_type_unit_fixtures DEPENDS ${type_unit_fixture_outputs})
add_dependencies(zxdb_tests zxdb_type_unit_fixtures)
# A Clang binary with inlined functions whose unused parameters Clang omits from the concrete
# instances. dwarf_symbol_factory_unittest.cc checks the order of the parameters.
set(inline_params_fixture_dir "${PROJECT_BINARY_DIR}/test_data/zxdb/inline_params")
set(inline_params_fixture_source
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/inline_params/inline_params_fixture.cc")
set(inline_params_fixture_output "${inline_params_fixture_dir}/clang_inline_params.so")
add_custom_command(
  OUTPUT "${inline_params_fixture_output}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${inline_params_fixture_dir}"
  COMMAND "${CMAKE_CXX_COMPILER}" -g -O2 -shared -fPIC -Wl,--build-id
    "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=../.." -o "${inline_params_fixture_output}"
    "${inline_params_fixture_source}"
  DEPENDS "${inline_params_fixture_source}"
  VERBATIM)
add_custom_target(zxdb_inline_params_fixtures DEPENDS "${inline_params_fixture_output}")
add_dependencies(zxdb_tests zxdb_inline_params_fixtures)

# A Clang binary whose functions use DW_FORM_LLVM_addrx_offset for DW_AT_low_pc.
# dwarf_symbol_factory_unittest.cc checks the function addresses.
set(addrx_offset_fixture_dir "${PROJECT_BINARY_DIR}/test_data/zxdb/addrx_offset")
set(addrx_offset_fixture_source
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/addrx_offset/addrx_offset_fixture.cc")
set(addrx_offset_fixture_output "${addrx_offset_fixture_dir}/clang_addrx_offset.so")
add_custom_command(
  OUTPUT "${addrx_offset_fixture_output}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${addrx_offset_fixture_dir}"
  COMMAND "${CMAKE_CXX_COMPILER}" -g -gdwarf-5 -O0 -mllvm -minimize-addr-in-v5=Form -shared -fPIC
    -Wl,--build-id "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=../.."
    -o "${addrx_offset_fixture_output}" "${addrx_offset_fixture_source}"
  DEPENDS "${addrx_offset_fixture_source}"
  VERBATIM)
add_custom_target(zxdb_addrx_offset_fixtures DEPENDS "${addrx_offset_fixture_output}")
add_dependencies(zxdb_tests zxdb_addrx_offset_fixtures)
# Exercise the same incomplete DWARF with and without the optional name accelerator.
set(function_name_source "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/function_names/functions.ll")
set(function_name_dir "${PROJECT_BINARY_DIR}/test_data/zxdb/function_names")
foreach(accelerator Disable Dwarf)
  set(output "${function_name_dir}/${accelerator}.so")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${function_name_dir}"
    COMMAND "${CMAKE_CXX_COMPILER}" -shared -nostdlib -Wl,--build-id -mllvm
      "-accel-tables=${accelerator}" "${function_name_source}" -o "${output}"
    DEPENDS "${function_name_source}"
    VERBATIM)
  list(APPEND function_name_outputs "${output}")
endforeach()
add_custom_target(zxdb_function_name_fixtures DEPENDS ${function_name_outputs})
add_dependencies(zxdb_tests zxdb_function_name_fixtures)
# Binaries from hand-written DWARF for dwarf_symbol_factory_unittest.cc:
#   - specification: DW_AT_specification chains and abstract origins in another unit.
#   - data_forms: array bounds and constants in the DW_FORM_data<n> forms.
set(assembly_fixture_outputs)
foreach(name specification data_forms)
  set(dir "${PROJECT_BINARY_DIR}/test_data/zxdb/${name}")
  set(source "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/${name}/${name}_fixture.s")
  set(output "${dir}/${name}.so")
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
    COMMAND "${CMAKE_CXX_COMPILER}" -shared -nostdlib -Wl,--build-id -o "${output}" "${source}"
    DEPENDS "${source}"
    VERBATIM)
  list(APPEND assembly_fixture_outputs "${output}")
endforeach()
add_custom_target(zxdb_assembly_fixtures DEPENDS ${assembly_fixture_outputs})
add_dependencies(zxdb_tests zxdb_assembly_fixtures)
# A .dwo file with a type unit that references the .debug_line.dwo table. Only the .dwo goes into the
# fixture directory: the object file has relocations that the comparison test doesn't apply.
set(line_table_split_dir "${PROJECT_BINARY_DIR}/test_data/zxdb/line_table_split")
set(line_table_split_source
  "${CMAKE_CURRENT_SOURCE_DIR}/symbols/test_data/line_table/line_table_split.cc")
add_custom_command(
  OUTPUT "${line_table_fixture_dir}/clang_split.dwo"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${line_table_split_dir}" "${line_table_fixture_dir}"
  COMMAND "${CMAKE_CXX_COMPILER}" -c -g -gdwarf-5 -gsplit-dwarf -fdebug-types-section
    "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=../.." -o "${line_table_split_dir}/clang_split.o"
    "${line_table_split_source}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${line_table_split_dir}/clang_split.dwo"
    "${line_table_fixture_dir}/clang_split.dwo"
  DEPENDS "${line_table_split_source}"
  VERBATIM)
list(APPEND line_table_fixture_outputs "${line_table_fixture_dir}/clang_split.dwo")
add_custom_target(zxdb_line_table_fixtures DEPENDS ${line_table_fixture_outputs})
add_dependencies(zxdb_tests zxdb_line_table_fixtures)

set(data_dir "${PROJECT_BINARY_DIR}/test_data/zxdb")
file(MAKE_DIRECTORY "${data_dir}/build_id/.build-id/9e" "${data_dir}/fake_home/.fuchsia/debug")
foreach(file IN ITEMS libsymbol_test_so.targetso libsymbol_test_so_stripped.targetso small_test_file.elf another.symbol-index.json symbol-index.json)
  configure_file("symbols/test_data/${file}" "${data_dir}/${file}" COPYONLY)
endforeach()
configure_file(symbols/test_data/libsymbol_test_so.targetso "${data_dir}/build_id/.build-id/9e/b335ffa35de20c.debug" COPYONLY)
file(COPY symbols/test_data/fission DESTINATION "${data_dir}")
foreach(file IN ITEMS test_example_minidump.dmp test_example_minidump_new_cvrecord.dmp test_example_minidump_with_aspace.dmp)
  configure_file("client/${file}" "${data_dir}/${file}" COPYONLY)
endforeach()
configure_file(client/test_dummy_file "${data_dir}/fake_home/.fuchsia/debug/googleapi_auth" COPYONLY)

include(GoogleTest)
# This optional Rust fixture is distributed separately by upstream CIPD.
set(ZXDB_RUST_TEST_BINARY "" CACHE FILEPATH "Optional upstream rust_symbol_test fixture")
set(zxdb_test_exclusions)
if(ZXDB_RUST_TEST_BINARY)
  configure_file("${ZXDB_RUST_TEST_BINARY}" "${data_dir}/rust_symbol_test.targetexe" COPYONLY)
else()
  list(APPEND zxdb_test_exclusions ModuleSymbols.ResolveLineInputLocation_Await)
endif()
set(ZXDB_SAMPLE_CORE_DIR "" CACHE PATH "Optional upstream sample_core fixture directory")
if(ZXDB_SAMPLE_CORE_DIR)
  file(COPY "${ZXDB_SAMPLE_CORE_DIR}/" DESTINATION "${data_dir}/sample_core")
else()
  list(APPEND zxdb_test_exclusions MinidumpTest.Backtrace)
endif()
list(JOIN zxdb_test_exclusions ":" zxdb_test_filter)
if(zxdb_test_filter)
  string(PREPEND zxdb_test_filter "-")
else()
  set(zxdb_test_filter "*")
endif()
gtest_discover_tests(zxdb_tests DISCOVERY_TIMEOUT 60 TEST_FILTER "${zxdb_test_filter}")

add_executable(debug_agent_tests
  ../debug_agent/align_unittest.cc
  ../debug_agent/aligned_chunk_unittest.cc
  ../debug_agent/debug_agent_filter_unittest.cc
  ../debug_agent/debug_agent_log_unittest.cc
  ../debug_agent/elf_utils_unittest.cc
  ../debug_agent/filter_unittest.cc
  ../debug_agent/linux_utils_unittest.cc
  ../debug_agent/linux_process_handle_unittest.cc
  ../debug_agent/linux_task_unittest.cc
  ../debug_agent/software_breakpoint_unittest.cc
  ../debug_agent/mock_thread.cc
  ../debug_agent/mock_process.cc
  ../debug_agent/mock_process_handle.cc
  ../debug_agent/mock_thread_handle.cc
  ../debug_agent/mock_limbo_provider.cc
  ../debug_agent/mock_job_handle.cc
  ../debug_agent/mock_system_interface.cc
  ../debug_agent/local_stream_backend.cc
  ../debug_agent/mock_component_manager.cc
  ../debug_agent/mock_debug_agent_harness.cc
  ../debug_agent/posix/file_descriptor_shuffle_unittest.cc
)
target_link_libraries(debug_agent_tests PRIVATE debug_agent debug_shared_test_support GTest::gtest_main GTest::gmock)
target_compile_definitions(debug_agent_tests PRIVATE LINUX_LIFECYCLE_INFERIOR="$<TARGET_FILE:linux_lifecycle_inferior>")
add_dependencies(debug_agent_tests linux_lifecycle_inferior)
gtest_discover_tests(debug_agent_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 15)

if(LLVM_VERSION_MAJOR EQUAL 19)
  set_source_files_properties(symbols/test_symbol_module.cc PROPERTIES COMPILE_OPTIONS -std=c++20)
endif()
