# Preserve upstream SDK include paths without changing imported source files.
set(sdk_include "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${sdk_include}/lib")
foreach(library IN ITEMS syslog utf-utils)
  file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/${library}"
    "${sdk_include}/lib/${library}" SYMBOLIC)
endforeach()

add_library(fuchsia_headers INTERFACE)
target_include_directories(fuchsia_headers INTERFACE "${PROJECT_SOURCE_DIR}")

add_library(zircon_headers INTERFACE)
target_include_directories(zircon_headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/zircon")

add_library(stdcompat INTERFACE)
target_include_directories(stdcompat INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stdcompat/include")

add_library(fit INTERFACE)
target_include_directories(fit INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/fit/include")
target_link_libraries(fit INTERFACE stdcompat)

add_library(fbl INTERFACE)
target_include_directories(fbl INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/fbl/include")

add_library(zx_result INTERFACE)
target_include_directories(zx_result INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/zx/include")
target_link_libraries(zx_result INTERFACE fit zircon_headers)

add_library(fxl STATIC fxl/strings/string_printf.cc)
target_link_libraries(fxl PUBLIC fuchsia_headers)

add_library(utf_utils STATIC utf-utils/utf-utils.cc utf-utils/internal/scalar.cc)
target_include_directories(utf_utils PUBLIC "${sdk_include}")
target_link_libraries(utf_utils PUBLIC stdcompat)

add_library(files STATIC files/path.cc files/directory.cc)
target_link_libraries(files PUBLIC fuchsia_headers PRIVATE fit utf_utils)

add_library(fpromise STATIC
  fit-promise/barrier.cc
  fit-promise/promise.cc
  fit-promise/scheduler.cc
  fit-promise/scope.cc
  fit-promise/sequencer.cc
  fit-promise/single_threaded_executor.cc
)
target_include_directories(fpromise PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/fit-promise/include")
target_link_libraries(fpromise PUBLIC fit Threads::Threads)

add_library(syslog STATIC
  syslog/cpp/log_message_impl.cc
  syslog/cpp/host/log_buffer.cc
  syslog/cpp/host/logging_backend.cc
)
target_include_directories(syslog PUBLIC "${sdk_include}")
target_link_libraries(syslog PUBLIC zx_result PRIVATE Threads::Threads)

# Additional host debugger support libraries.
target_sources(fxl PRIVATE
  fxl/memory/weak_ptr_internal.cc
  fxl/strings/ascii.cc
  fxl/strings/split_string.cc
  fxl/strings/string_number_conversions.cc
  fxl/strings/substitute.cc
  fxl/strings/trim.cc
  fxl/strings/utf_codecs.cc
  fxl/third_party/icu/icu_utf.cc
)
target_link_libraries(fxl PUBLIC zircon_headers syslog utf_utils)
target_sources(files PRIVATE
  files/file.cc
  files/file_descriptor.cc
  files/glob.cc
  files/scoped_temp_dir.cc
)
target_link_libraries(files PUBLIC fxl fbl syslog)

add_library(safemath INTERFACE)
target_include_directories(safemath INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/safemath/include")
add_library(containers INTERFACE)
target_link_libraries(containers INTERFACE fuchsia_headers safemath)
add_library(cmdline STATIC cmdline/args_parser.cc)
target_include_directories(cmdline PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cmdline/include")
add_library(elflib STATIC elflib/elflib.cc)
target_link_libraries(elflib PUBLIC llvm_host fbl fuchsia_headers zircon_headers)
add_library(uuid STATIC uuid/uuid.cc)
target_link_libraries(uuid PUBLIC fxl)
add_library(line_input STATIC line_input/line_input.cc line_input/modal_line_input.cc)
target_link_libraries(line_input PUBLIC fit syslog fxl)
add_library(unwinder STATIC
  unwinder/arm_ehabi_module.cc
  unwinder/arm_ehabi_parser.cc
  unwinder/arm_ehabi_unwinder.cc
  unwinder/cfi_module.cc
  unwinder/cfi_parser.cc
  unwinder/cfi_unwinder.cc
  unwinder/dwarf_expr.cc
  unwinder/elf_module_cache.cc
  unwinder/elf_utils.cc
  unwinder/error.cc
  unwinder/fp_unwinder.cc
  unwinder/linux.cc
  unwinder/loaded_elf_module.cc
  unwinder/memory.cc
  unwinder/module.cc
  unwinder/plt_unwinder.cc
  unwinder/registers.cc
  unwinder/scs_unwinder.cc
  unwinder/sigreturn_unwinder.cc
  unwinder/unwind.cc
  unwinder/unwinder_base.cc
)
target_link_libraries(unwinder PUBLIC fit safemath fuchsia_headers gtest_headers)
add_library(analytics STATIC
  analytics/cpp/core_dev_tools/analytics_executor.cc
  analytics/cpp/core_dev_tools/analytics_internal.cc
  analytics/cpp/core_dev_tools/analytics_messages.cc
  analytics/cpp/core_dev_tools/command_line_options.cc
  analytics/cpp/core_dev_tools/environment_status.cc
  analytics/cpp/core_dev_tools/google_analytics_4_client.cc
  analytics/cpp/core_dev_tools/persistent_status.cc
  analytics/cpp/core_dev_tools/system_info.cc
  analytics/cpp/google_analytics_4/batch.cc
  analytics/cpp/google_analytics_4/client.cc
  analytics/cpp/google_analytics_4/event.cc
  analytics/cpp/google_analytics_4/measurement.cc
  analytics/cpp/metric_properties/metric_properties.cc
  analytics/cpp/metric_properties/optional_path.cc
)
target_link_libraries(analytics PUBLIC zxdb_common uuid cmdline files fxl rapidjson)
