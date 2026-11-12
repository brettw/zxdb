add_library(crashpad STATIC
  snapshot/annotation_snapshot.cc
  snapshot/capture_memory.cc
  snapshot/cpu_context.cc
  snapshot/crashpad_info_client_options.cc
  snapshot/handle_snapshot.cc
  snapshot/memory_snapshot.cc
  snapshot/minidump/exception_snapshot_minidump.cc
  snapshot/minidump/memory_snapshot_minidump.cc
  snapshot/minidump/minidump_annotation_reader.cc
  snapshot/minidump/minidump_context_converter.cc
  snapshot/minidump/minidump_simple_string_dictionary_reader.cc
  snapshot/minidump/minidump_string_list_reader.cc
  snapshot/minidump/minidump_string_reader.cc
  snapshot/minidump/module_snapshot_minidump.cc
  snapshot/minidump/process_snapshot_minidump.cc
  snapshot/minidump/system_snapshot_minidump.cc
  snapshot/minidump/thread_snapshot_minidump.cc
  snapshot/posix/timezone.cc
  snapshot/unloaded_module_snapshot.cc
  util/file/file_io.cc
  util/file/file_io_posix.cc
  util/file/file_reader.cc
  util/file/file_seeker.cc
  util/misc/clock_posix.cc
  util/misc/initialization_state_dcheck.cc
  util/misc/pdb_structures.cc
  util/misc/random_string.cc
  util/misc/time.cc
  util/misc/uuid.cc
  util/process/process_memory.cc
)
target_include_directories(crashpad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(crashpad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/compat/non_win" "${CMAKE_CURRENT_SOURCE_DIR}/compat/linux")
target_link_libraries(crashpad PUBLIC mini_chromium Threads::Threads ZLIB::ZLIB)
