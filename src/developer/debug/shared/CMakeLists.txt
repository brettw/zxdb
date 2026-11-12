add_library(debug_logging STATIC
  logging/debug.cc
  logging/file_line_function.cc
  logging/logging.cc
)
target_link_libraries(debug_logging PUBLIC fuchsia_headers PRIVATE syslog fxl files)

# Keep this source list aligned with BUILD.bazel's shared target on Linux.
add_library(debug_shared STATIC
  address_range.cc
  arch.cc
  arch_x86.cc
  buffered_bidi_pipe.cc
  buffered_fd.cc
  handle_info.cc
  message_loop.cc
  platform.cc
  regex.cc
  register_info.cc
  register_value.cc
  serialization.cc
  status.cc
  stream_buffer.cc
  string_util.cc
  test_stream_buffer.cc
  zx_status.cc
  message_loop_linux.cc
  message_loop_poll.cc
)
target_compile_features(debug_shared PUBLIC cxx_std_20)
target_link_libraries(debug_shared
  PUBLIC fpromise syslog "$<LINK_LIBRARY:WHOLE_ARCHIVE,debug_logging>" fxl fbl zircon_headers re2::re2
  PRIVATE files Threads::Threads
)

# Match the upstream alwayslink behavior for shared and logging. Keep the
# archive target separately available for builds of the library alone.
add_library(debug_shared_usage INTERFACE)
target_link_libraries(debug_shared_usage INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,debug_shared>")
add_library(debug::shared ALIAS debug_shared_usage)

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  add_library(debug_shared_test_support STATIC mock_memory.cc register_test_support.cc)
  target_link_libraries(debug_shared_test_support PUBLIC debug::shared GTest::gtest)

  add_executable(shared_tests
    address_range_unittest.cc
    handle_info_unittest.cc
    largest_less_or_equal_unittest.cc
    message_loop_unittest.cc
    regex_unittest.cc
    register_info_unittest.cc
    stream_buffer_unittest.cc
    string_util_unittest.cc
    message_loop_linux_unittest.cc
  )
  target_link_libraries(shared_tests PRIVATE debug::shared GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(shared_tests DISCOVERY_TIMEOUT 30)
endif()
