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
