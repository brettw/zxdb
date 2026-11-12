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
  symbols/dwarf_abstract_child_iterator_unittest.cc
  symbols/dwarf_debug_names_unittest.cc
  symbols/dwarf_die_ref_unittest.cc
  symbols/dwarf_expr_eval_unittest.cc
  symbols/dwarf_location_unittest.cc
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
  symbols/line_table_unittest.cc
  symbols/loaded_module_symbols_unittest.cc
  symbols/member_ptr_unittest.cc
  symbols/mock_line_table.cc
  symbols/mock_module_symbols.cc
  symbols/mock_source_file_provider.cc
  symbols/mock_symbol_data_provider.cc
  symbols/mock_symbol_data_provider_unittest.cc
  symbols/modified_type_unittest.cc
  symbols/module_symbols_impl_unittest.cc
  symbols/process_symbols_test_setup.cc
  symbols/process_symbols_unittest.cc
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
  ../debug_agent/elf_utils_unittest.cc
  ../debug_agent/linux_utils_unittest.cc
  ../debug_agent/posix/file_descriptor_shuffle_unittest.cc
)
target_link_libraries(debug_agent_tests PRIVATE debug_agent debug_shared_test_support GTest::gtest_main GTest::gmock)
gtest_discover_tests(debug_agent_tests DISCOVERY_TIMEOUT 30)

if(LLVM_VERSION_MAJOR EQUAL 19)
  set_source_files_properties(symbols/test_symbol_module.cc PROPERTIES COMPILE_OPTIONS -std=c++20)
endif()
