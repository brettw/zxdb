add_library(mini_chromium STATIC
  base/debug/alias.cc
  base/files/file_path.cc
  base/files/file_util_posix.cc
  base/files/scoped_file.cc
  base/logging.cc
  base/memory/page_size_posix.cc
  base/posix/safe_strerror.cc
  base/process/memory.cc
  base/rand_util.cc
  base/strings/pattern.cc
  base/strings/strcat.cc
  base/strings/string_number_conversions.cc
  base/strings/stringprintf.cc
  base/strings/utf_string_conversion_utils.cc
  base/strings/utf_string_conversions.cc
  base/synchronization/condition_variable_posix.cc
  base/synchronization/lock.cc
  base/synchronization/lock_impl_posix.cc
  base/third_party/icu/icu_utf.cc
  base/threading/thread_local_storage.cc
  base/threading/thread_local_storage_posix.cc
)
target_include_directories(mini_chromium PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mini_chromium PUBLIC Threads::Threads)
