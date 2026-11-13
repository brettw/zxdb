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

add_executable(linux_lifecycle_inferior linux_lifecycle_inferior.cc)
target_link_libraries(linux_lifecycle_inferior PRIVATE pthread)
target_compile_options(linux_lifecycle_inferior PRIVATE -O0 -g -gdwarf-4 -fno-pie)
target_link_options(linux_lifecycle_inferior PRIVATE -no-pie)

foreach(LIFECYCLE_MODE IN ITEMS threads leader-exit exec worker-exec output stepi vfork vfork-exit)
  if(LIFECYCLE_MODE STREQUAL "stepi")
    configure_file(local_stepi.script.in local_${LIFECYCLE_MODE}.script @ONLY)
  elseif(LIFECYCLE_MODE STREQUAL "output")
    configure_file(local_output.script.in local_${LIFECYCLE_MODE}.script @ONLY)
  else()
    configure_file(local_lifecycle.script.in local_${LIFECYCLE_MODE}.script @ONLY)
  endif()
  add_test(NAME local_${LIFECYCLE_MODE}
    COMMAND zxdb --local --enable-debug-adapter --debug-adapter-port=0
      --symbol-cache=${CMAKE_CURRENT_BINARY_DIR}/symbol-cache
      --symbol-path=$<TARGET_FILE:linux_lifecycle_inferior>
      --script-file=${CMAKE_CURRENT_BINARY_DIR}/local_${LIFECYCLE_MODE}.script)
  set_tests_properties(local_${LIFECYCLE_MODE} PROPERTIES
    ENVIRONMENT FUCHSIA_ANALYTICS_DISABLED=1 TIMEOUT 15 LABELS integration
    FAIL_REGULAR_EXPRESSION "unexpected process")
endforeach()

foreach(SCENARIO IN ITEMS fault_step vfork_breakpoint exec_breakpoint)
  configure_file(local_${SCENARIO}.script.in local_${SCENARIO}.script @ONLY)
  add_test(NAME local_${SCENARIO}
    COMMAND zxdb --local --enable-debug-adapter --debug-adapter-port=0
      --symbol-cache=${CMAKE_CURRENT_BINARY_DIR}/symbol-cache
      --symbol-path=$<TARGET_FILE:linux_lifecycle_inferior>
      --script-file=${CMAKE_CURRENT_BINARY_DIR}/local_${SCENARIO}.script)
  set_tests_properties(local_${SCENARIO} PROPERTIES
    ENVIRONMENT FUCHSIA_ANALYTICS_DISABLED=1 TIMEOUT 15 LABELS integration)
endforeach()

foreach(SHUTDOWN_MODE IN ITEMS quit detach frontend-crash)
  add_test(NAME local_shutdown_${SHUTDOWN_MODE}
    COMMAND linux_regression_fixture --shutdown ${SHUTDOWN_MODE}
      $<TARGET_FILE:zxdb> $<TARGET_FILE:linux_lifecycle_inferior>)
  set_tests_properties(local_shutdown_${SHUTDOWN_MODE} PROPERTIES
    ENVIRONMENT FUCHSIA_ANALYTICS_DISABLED=1 TIMEOUT 20 LABELS integration)
endforeach()
