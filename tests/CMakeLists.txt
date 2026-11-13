add_executable(local_inferior local_inferior.cc)
target_compile_options(local_inferior PRIVATE -O0 -g -gdwarf-4 -fno-omit-frame-pointer)
target_link_options(local_inferior PRIVATE -Wl,--build-id)
configure_file(local.script.in local.script @ONLY)
add_test(NAME zxdb_version COMMAND zxdb --version)
set_tests_properties(zxdb_version PROPERTIES PASS_REGULAR_EXPRESSION "Version: [0-9]+")
add_test(NAME local_debugging
  COMMAND zxdb --local --enable-debug-adapter --debug-adapter-port=0
    --symbol-cache=${CMAKE_CURRENT_BINARY_DIR}/symbol-cache
    --symbol-path=$<TARGET_FILE:local_inferior>
    --script-file=${CMAKE_CURRENT_BINARY_DIR}/local.script)
set_tests_properties(local_debugging PROPERTIES
  ENVIRONMENT FUCHSIA_ANALYTICS_DISABLED=1
  TIMEOUT 45
  LABELS integration)

configure_file(local_exit.script.in local_exit.script @ONLY)
add_test(NAME local_nonzero_exit
  COMMAND zxdb --local --enable-debug-adapter --debug-adapter-port=0
    --symbol-cache=${CMAKE_CURRENT_BINARY_DIR}/symbol-cache
    --script-file=${CMAKE_CURRENT_BINARY_DIR}/local_exit.script)
set_tests_properties(local_nonzero_exit PROPERTIES
  ENVIRONMENT FUCHSIA_ANALYTICS_DISABLED=1 TIMEOUT 45 LABELS integration)

add_executable(linux_regression_fixture linux_regression_fixture.cc)
target_link_libraries(linux_regression_fixture PRIVATE debug_agent)
add_test(NAME linux_auxv_unreadable_executable COMMAND linux_regression_fixture --auxv)
set_tests_properties(linux_auxv_unreadable_executable PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)
configure_file(local_ignored.script.in local_ignored.script @ONLY)
foreach(disposition IN ITEMS default ignored)
  if(disposition STREQUAL "ignored")
    set(signal_script local_ignored.script)
  else()
    set(signal_script local_exit.script)
  endif()
  add_test(NAME local_sigpipe_${disposition}
    COMMAND linux_regression_fixture --sigpipe ${disposition} $<TARGET_FILE:zxdb>
      --local --enable-debug-adapter --debug-adapter-port=0
      --symbol-cache=${CMAKE_CURRENT_BINARY_DIR}/symbol-cache
      --script-file=${CMAKE_CURRENT_BINARY_DIR}/${signal_script})
  set_tests_properties(local_sigpipe_${disposition} PROPERTIES
    ENVIRONMENT FUCHSIA_ANALYTICS_DISABLED=1 TIMEOUT 45 LABELS integration)
endforeach()
