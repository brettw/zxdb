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
